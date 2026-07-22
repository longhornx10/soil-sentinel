"""ZHA quirk for longhornx10 Soil Sentinel.

Configuration writes are cached controller-side. A failed write is retried when the
next telemetry report proves that the deeply sleeping sensor is awake.
"""

from __future__ import annotations

import asyncio
import logging
from typing import Any, Final

import zigpy.types as t
from zigpy.zcl import foundation
from zigpy.zcl.foundation import BaseAttributeDefs, ZCLAttributeDef

from zhaquirks import CustomCluster
from zhaquirks.builder import (
    PERCENTAGE,
    BinarySensorDeviceClass,
    EntityPlatform,
    EntityType,
    QuirkBuilder,
    SensorDeviceClass,
    SensorStateClass,
    UnitOfElectricPotential,
    UnitOfTime,
)

_LOGGER = logging.getLogger(__name__)

TELEMETRY_CLUSTER_ID = 0xFC00
CONTROL_CLUSTER_ID = 0xFC01


class SoilSentinelMode(t.enum8):
    Stable = 0
    Drying = 1
    Near_dry = 2
    Critical = 3
    Watering_capture = 4
    Recently_watered = 5
    Conservation = 6
    Survival = 7


class SoilCalibrationMode(t.enum8):
    Stock = 0
    Learning = 1
    Manual = 2


class SoilCurveSource(t.enum8):
    Stock = 0
    Learned = 1
    Manual = 2
    Fallback_stock = 3


class SoilConfigResult(t.enum8):
    Ok = 0
    Reject_mode = 1
    Reject_adc_range = 2
    Reject_span = 3
    Reject_thresholds = 4
    Reject_no_sample = 5
    Reject_no_learned_curve = 6
    Reject_persistence = 7


class SoilOtaState(t.enum8):
    Idle = 0
    Armed = 1
    Querying = 2
    Downloading = 3
    Applying = 4
    Complete = 5
    Failed = 6
    Refused = 7


class SoilOtaResult(t.enum8):
    None_ = 0
    Success = 1
    No_image = 2
    Low_battery = 3
    Download_error = 4
    Validation_error = 5
    Rolled_back = 6
    Timeout = 7


class SoilSentinelEventFlags(t.bitmap16):
    Threshold = 1 << 0
    Watering = 1 << 1
    Fault = 1 << 2
    Heartbeat = 1 << 3
    Battery = 1 << 4
    Manual = 1 << 5
    Mode = 1 << 6
    Config = 1 << 7


class SoilSentinelControlCluster(CustomCluster):
    """Writable configuration cluster with sleepy-device retry support."""

    cluster_id = CONTROL_CLUSTER_ID
    ep_attribute = "soil_sentinel_control"

    DESIRED_FIELDS = (0x0000, 0x0001, 0x0002, 0x0003, 0x0004)
    DESIRED_REVISION = 0x0005
    ACTION = 0x0006
    APPLIED_REVISION = 0x0010

    class AttributeDefs(BaseAttributeDefs):
        desired_mode: Final = ZCLAttributeDef(id=0x0000, type=SoilCalibrationMode, access="rw")
        desired_manual_dry: Final = ZCLAttributeDef(id=0x0001, type=t.uint16_t, access="rw")
        desired_manual_wet: Final = ZCLAttributeDef(id=0x0002, type=t.uint16_t, access="rw")
        desired_dry_threshold: Final = ZCLAttributeDef(id=0x0003, type=t.uint8_t, access="rw")
        desired_critical_threshold: Final = ZCLAttributeDef(id=0x0004, type=t.uint8_t, access="rw")
        desired_revision: Final = ZCLAttributeDef(id=0x0005, type=t.uint32_t, access="rw")
        action: Final = ZCLAttributeDef(id=0x0006, type=t.bitmap16, access="rw")
        applied_revision: Final = ZCLAttributeDef(id=0x0010, type=t.uint32_t, access="r")
        config_result: Final = ZCLAttributeDef(id=0x0011, type=SoilConfigResult, access="r")
        active_mode: Final = ZCLAttributeDef(id=0x0012, type=SoilCalibrationMode, access="r")
        active_dry: Final = ZCLAttributeDef(id=0x0013, type=t.uint16_t, access="r")
        active_wet: Final = ZCLAttributeDef(id=0x0014, type=t.uint16_t, access="r")
        learned_dry: Final = ZCLAttributeDef(id=0x0015, type=t.uint16_t, access="r")
        learned_wet: Final = ZCLAttributeDef(id=0x0016, type=t.uint16_t, access="r")
        learning_confidence: Final = ZCLAttributeDef(id=0x0017, type=t.uint8_t, access="r")
        learning_cycles: Final = ZCLAttributeDef(id=0x0018, type=t.uint16_t, access="r")

    def __init__(self, *args: Any, **kwargs: Any) -> None:
        super().__init__(*args, **kwargs)
        self._pending_config: dict[int, Any] | None = None
        self._pending_action: dict[int, Any] | None = None
        self._flush_lock = asyncio.Lock()

    def _attribute_id(self, key: str | int | ZCLAttributeDef) -> int:
        if isinstance(key, int):
            return key
        if isinstance(key, ZCLAttributeDef):
            return key.id
        return self.attributes_by_name[key].id

    @staticmethod
    def _success_response() -> list[list[foundation.WriteAttributesStatusRecord]]:
        return [[foundation.WriteAttributesStatusRecord(status=foundation.Status.SUCCESS)]]

    def _cached(self, attrid: int, default: Any) -> Any:
        return self._attr_cache.get(attrid, default)

    def _build_full_config(self, changed: dict[int, Any]) -> dict[int, Any]:
        config = {
            attrid: changed.get(attrid, self._cached(attrid, 0))
            for attrid in self.DESIRED_FIELDS
        }
        current_desired = int(self._cached(self.DESIRED_REVISION, 0))
        applied = int(self._cached(self.APPLIED_REVISION, 0))
        config[self.DESIRED_REVISION] = max(current_desired, applied) + 1
        return config

    async def _send_or_queue(self, payload: dict[int, Any], *, action: bool = False):
        try:
            result = await super().write_attributes(payload)
        except Exception as exc:  # sleepy devices are expected to be absent here
            _LOGGER.debug("Soil Sentinel asleep; queued write %s: %s", payload, exc)
            if action:
                self._pending_action = payload
            else:
                self._pending_config = payload
            return self._success_response()
        if action:
            self._pending_action = None
        else:
            self._pending_config = None
        return result

    async def write_attributes(
        self,
        attributes: dict[str | int | ZCLAttributeDef, Any],
        **kwargs: Any,
    ) -> list[list[foundation.WriteAttributesStatusRecord]]:
        normalized = {self._attribute_id(key): value for key, value in attributes.items()}
        if self.ACTION in normalized:
            self._update_attribute(self.ACTION, normalized[self.ACTION])
            return await self._send_or_queue({self.ACTION: normalized[self.ACTION]}, action=True)

        changed = {key: value for key, value in normalized.items() if key in self.DESIRED_FIELDS}
        if not changed:
            return await super().write_attributes(attributes, **kwargs)

        payload = self._build_full_config(changed)
        for attrid, value in payload.items():
            self._update_attribute(attrid, value)
        self._pending_config = payload
        return await self._send_or_queue(payload)

    def reconstruct_pending_from_cache(self, applied_revision: int) -> None:
        desired_revision = int(self._cached(self.DESIRED_REVISION, 0))
        if desired_revision <= applied_revision or self._pending_config is not None:
            return
        self._pending_config = {
            attrid: self._cached(attrid, 0) for attrid in self.DESIRED_FIELDS
        }
        self._pending_config[self.DESIRED_REVISION] = desired_revision

    async def flush_pending(self) -> None:
        async with self._flush_lock:
            if self._pending_config:
                payload = dict(self._pending_config)
                await self._send_or_queue(payload)
            if self._pending_action:
                payload = dict(self._pending_action)
                await self._send_or_queue(payload, action=True)


class SoilSentinelTelemetryCluster(CustomCluster):
    """Compact telemetry frame unpacked into local ZHA attributes."""

    cluster_id = TELEMETRY_CLUSTER_ID
    ep_attribute = "soil_sentinel_telemetry"
    TELEMETRY_SCHEMA_VERSION = 2
    TELEMETRY_PAYLOAD_LENGTH = 48
    REPORT_REASON_MASK = 0x00FF
    STATUS_CURRENT_SAMPLE_VALID = 1 << 8
    STATUS_HAS_VALID_MOISTURE = 1 << 9
    STATUS_HAS_WATERED = 1 << 10

    class AttributeDefs(BaseAttributeDefs):
        telemetry: Final = ZCLAttributeDef(id=0x0000, type=t.LVBytes, access="rp")
        raw_probe_voltage: Final = ZCLAttributeDef(id=0x0100, type=t.uint16_t, access="r")
        measurement_noise: Final = ZCLAttributeDef(id=0x0101, type=t.uint16_t, access="r")
        confidence: Final = ZCLAttributeDef(id=0x0102, type=t.uint8_t, access="r")
        drying_rate: Final = ZCLAttributeDef(id=0x0103, type=t.int16s, access="r")
        sample_interval: Final = ZCLAttributeDef(id=0x0104, type=t.uint32_t, access="r")
        seconds_since_watering: Final = ZCLAttributeDef(id=0x0105, type=t.uint32_t, access="r")
        operating_mode: Final = ZCLAttributeDef(id=0x0106, type=SoilSentinelMode, access="r")
        report_reason: Final = ZCLAttributeDef(id=0x0107, type=SoilSentinelEventFlags, access="r")
        sensor_fault: Final = ZCLAttributeDef(id=0x0108, type=t.Bool, access="r")
        battery_present: Final = ZCLAttributeDef(id=0x0109, type=t.Bool, access="r")
        current_sample_valid: Final = ZCLAttributeDef(id=0x010A, type=t.Bool, access="r")
        has_valid_moisture: Final = ZCLAttributeDef(id=0x010B, type=t.Bool, access="r")
        has_watered: Final = ZCLAttributeDef(id=0x010C, type=t.Bool, access="r")
        calibration_mode: Final = ZCLAttributeDef(id=0x0110, type=SoilCalibrationMode, access="r")
        active_curve_source: Final = ZCLAttributeDef(id=0x0111, type=SoilCurveSource, access="r")
        config_result: Final = ZCLAttributeDef(id=0x0112, type=SoilConfigResult, access="r")
        active_dry_voltage: Final = ZCLAttributeDef(id=0x0113, type=t.uint16_t, access="r")
        active_wet_voltage: Final = ZCLAttributeDef(id=0x0114, type=t.uint16_t, access="r")
        learned_dry_voltage: Final = ZCLAttributeDef(id=0x0115, type=t.uint16_t, access="r")
        learned_wet_voltage: Final = ZCLAttributeDef(id=0x0116, type=t.uint16_t, access="r")
        learning_confidence: Final = ZCLAttributeDef(id=0x0117, type=t.uint8_t, access="r")
        learning_cycles: Final = ZCLAttributeDef(id=0x0118, type=t.uint16_t, access="r")
        applied_config_revision: Final = ZCLAttributeDef(id=0x0119, type=t.uint32_t, access="r")
        configuration_pending: Final = ZCLAttributeDef(id=0x011A, type=t.Bool, access="r")
        ota_state: Final = ZCLAttributeDef(id=0x0120, type=SoilOtaState, access="r")
        ota_result: Final = ZCLAttributeDef(id=0x0121, type=SoilOtaResult, access="r")
        ota_progress: Final = ZCLAttributeDef(id=0x0122, type=t.uint8_t, access="r")
        active_ota_slot: Final = ZCLAttributeDef(id=0x0123, type=t.uint8_t, access="r")
        rollback_pending: Final = ZCLAttributeDef(id=0x0124, type=t.Bool, access="r")
        firmware_file_version: Final = ZCLAttributeDef(id=0x0125, type=t.uint32_t, access="r")

    def _update_attribute(self, attrid: int, value: Any) -> None:
        super()._update_attribute(attrid, value)
        if attrid != self.AttributeDefs.telemetry.id:
            return
        payload = bytes(value)
        if len(payload) != self.TELEMETRY_PAYLOAD_LENGTH or payload[0] != 2:
            return

        flags = int.from_bytes(payload[2:4], "little")
        applied_revision = int.from_bytes(payload[35:39], "little")
        self._update_attribute(self.AttributeDefs.operating_mode.id, SoilSentinelMode(payload[1]))
        self._update_attribute(self.AttributeDefs.report_reason.id, SoilSentinelEventFlags(flags & 0xFF))
        self._update_attribute(self.AttributeDefs.sensor_fault.id, bool(payload[4]))
        self._update_attribute(self.AttributeDefs.confidence.id, payload[5])
        self._update_attribute(self.AttributeDefs.raw_probe_voltage.id, int.from_bytes(payload[6:8], "little"))
        self._update_attribute(self.AttributeDefs.measurement_noise.id, int.from_bytes(payload[8:10], "little"))
        self._update_attribute(self.AttributeDefs.drying_rate.id, int.from_bytes(payload[10:12], "little", signed=True))
        self._update_attribute(self.AttributeDefs.sample_interval.id, int.from_bytes(payload[12:16], "little"))
        self._update_attribute(self.AttributeDefs.seconds_since_watering.id, int.from_bytes(payload[16:20], "little"))
        self._update_attribute(self.AttributeDefs.battery_present.id, bool(payload[20]))
        self._update_attribute(self.AttributeDefs.current_sample_valid.id, bool(flags & self.STATUS_CURRENT_SAMPLE_VALID))
        self._update_attribute(self.AttributeDefs.has_valid_moisture.id, bool(flags & self.STATUS_HAS_VALID_MOISTURE))
        self._update_attribute(self.AttributeDefs.has_watered.id, bool(flags & self.STATUS_HAS_WATERED))
        self._update_attribute(self.AttributeDefs.calibration_mode.id, SoilCalibrationMode(payload[21]))
        self._update_attribute(self.AttributeDefs.active_curve_source.id, SoilCurveSource(payload[22]))
        self._update_attribute(self.AttributeDefs.config_result.id, SoilConfigResult(payload[23]))
        self._update_attribute(self.AttributeDefs.active_dry_voltage.id, int.from_bytes(payload[24:26], "little"))
        self._update_attribute(self.AttributeDefs.active_wet_voltage.id, int.from_bytes(payload[26:28], "little"))
        self._update_attribute(self.AttributeDefs.learned_dry_voltage.id, int.from_bytes(payload[28:30], "little"))
        self._update_attribute(self.AttributeDefs.learned_wet_voltage.id, int.from_bytes(payload[30:32], "little"))
        self._update_attribute(self.AttributeDefs.learning_confidence.id, payload[32])
        self._update_attribute(self.AttributeDefs.learning_cycles.id, int.from_bytes(payload[33:35], "little"))
        self._update_attribute(self.AttributeDefs.applied_config_revision.id, applied_revision)
        self._update_attribute(self.AttributeDefs.ota_state.id, SoilOtaState(payload[39]))
        self._update_attribute(self.AttributeDefs.ota_result.id, SoilOtaResult(payload[40]))
        self._update_attribute(self.AttributeDefs.ota_progress.id, payload[41])
        self._update_attribute(self.AttributeDefs.active_ota_slot.id, payload[42])
        self._update_attribute(self.AttributeDefs.rollback_pending.id, bool(payload[43]))
        self._update_attribute(self.AttributeDefs.firmware_file_version.id, int.from_bytes(payload[44:48], "little"))

        control = self.endpoint.in_clusters.get(CONTROL_CLUSTER_ID)
        desired_revision = applied_revision
        if isinstance(control, SoilSentinelControlCluster):
            control._update_attribute(control.APPLIED_REVISION, applied_revision)
            control.reconstruct_pending_from_cache(applied_revision)
            desired_revision = int(control._attr_cache.get(control.DESIRED_REVISION, applied_revision))
            try:
                asyncio.get_running_loop().create_task(control.flush_pending())
            except RuntimeError:
                pass
        self._update_attribute(self.AttributeDefs.configuration_pending.id, desired_revision > applied_revision)


builder = (
    QuirkBuilder("longhornx10", "Soil Sentinel")
    .replaces(SoilSentinelTelemetryCluster)
    .replaces(SoilSentinelControlCluster)
    .enum(
        SoilSentinelControlCluster.AttributeDefs.desired_mode.name,
        SoilCalibrationMode,
        CONTROL_CLUSTER_ID,
        entity_platform=EntityPlatform.SELECT,
        translation_key="calibration_mode",
        fallback_name="Calibration mode",
    )
    .number(
        SoilSentinelControlCluster.AttributeDefs.desired_manual_dry.name,
        CONTROL_CLUSTER_ID,
        min_value=50,
        max_value=3300,
        step=10,
        unit=UnitOfElectricPotential.MILLIVOLT,
        translation_key="manual_dry_voltage",
        fallback_name="Manual dry voltage",
    )
    .number(
        SoilSentinelControlCluster.AttributeDefs.desired_manual_wet.name,
        CONTROL_CLUSTER_ID,
        min_value=50,
        max_value=3300,
        step=10,
        unit=UnitOfElectricPotential.MILLIVOLT,
        translation_key="manual_wet_voltage",
        fallback_name="Manual wet voltage",
    )
    .number(
        SoilSentinelControlCluster.AttributeDefs.desired_dry_threshold.name,
        CONTROL_CLUSTER_ID,
        min_value=1,
        max_value=99,
        step=1,
        unit=PERCENTAGE,
        translation_key="dry_threshold",
        fallback_name="Dry threshold",
    )
    .number(
        SoilSentinelControlCluster.AttributeDefs.desired_critical_threshold.name,
        CONTROL_CLUSTER_ID,
        min_value=0,
        max_value=98,
        step=1,
        unit=PERCENTAGE,
        translation_key="critical_threshold",
        fallback_name="Critical threshold",
    )
    .write_attr_button(
        SoilSentinelControlCluster.AttributeDefs.action.name,
        CONTROL_CLUSTER_ID,
        attribute_value=1,
        unique_id_suffix="use_current_as_dry",
        translation_key="use_current_as_dry",
        fallback_name="Use current reading as dry",
    )
    .write_attr_button(
        SoilSentinelControlCluster.AttributeDefs.action.name,
        CONTROL_CLUSTER_ID,
        attribute_value=2,
        unique_id_suffix="use_current_as_wet",
        translation_key="use_current_as_wet",
        fallback_name="Use current reading as wet",
    )
    .write_attr_button(
        SoilSentinelControlCluster.AttributeDefs.action.name,
        CONTROL_CLUSTER_ID,
        attribute_value=4,
        unique_id_suffix="copy_learned_to_manual",
        translation_key="copy_learned_to_manual",
        fallback_name="Copy learned bounds to manual",
    )
    .write_attr_button(
        SoilSentinelControlCluster.AttributeDefs.action.name,
        CONTROL_CLUSTER_ID,
        attribute_value=8,
        unique_id_suffix="reset_learning",
        translation_key="reset_learning",
        fallback_name="Reset learned curve",
    )
    .write_attr_button(
        SoilSentinelControlCluster.AttributeDefs.action.name,
        CONTROL_CLUSTER_ID,
        attribute_value=16,
        unique_id_suffix="restore_manual_stock",
        translation_key="restore_manual_stock",
        fallback_name="Restore manual bounds to stock",
    )
    .write_attr_button(
        SoilSentinelControlCluster.AttributeDefs.action.name,
        CONTROL_CLUSTER_ID,
        attribute_value=32,
        unique_id_suffix="plant_moved",
        translation_key="plant_moved",
        fallback_name="Plant moved / restart learning",
    )
    .write_attr_button(
        SoilSentinelControlCluster.AttributeDefs.action.name,
        CONTROL_CLUSTER_ID,
        attribute_value=64,
        unique_id_suffix="identify",
        translation_key="identify",
        fallback_name="Identify sensor",
    )
)

for attr, enum_type, key, name in (
    ("operating_mode", SoilSentinelMode, "operating_mode", "Operating mode"),
    ("calibration_mode", SoilCalibrationMode, "active_calibration_mode", "Active calibration mode"),
    ("active_curve_source", SoilCurveSource, "active_curve_source", "Active curve source"),
    ("config_result", SoilConfigResult, "config_result", "Last configuration result"),
    ("ota_state", SoilOtaState, "ota_state", "OTA state"),
    ("ota_result", SoilOtaResult, "ota_result", "Last OTA result"),
):
    builder = builder.enum(
        attr,
        enum_type,
        TELEMETRY_CLUSTER_ID,
        entity_platform=EntityPlatform.SENSOR,
        entity_type=EntityType.DIAGNOSTIC,
        translation_key=key,
        fallback_name=name,
    )

builder = (
    builder
    .binary_sensor("sensor_fault", TELEMETRY_CLUSTER_ID,
        device_class=BinarySensorDeviceClass.PROBLEM,
        entity_type=EntityType.DIAGNOSTIC,
        fallback_name="Sensor fault")
    .binary_sensor("current_sample_valid", TELEMETRY_CLUSTER_ID,
        entity_type=EntityType.DIAGNOSTIC,
        fallback_name="Current measurement valid")
    .binary_sensor("has_watered", TELEMETRY_CLUSTER_ID,
        entity_type=EntityType.DIAGNOSTIC,
        fallback_name="Watering observed")
    .binary_sensor("configuration_pending", TELEMETRY_CLUSTER_ID,
        entity_type=EntityType.DIAGNOSTIC,
        fallback_name="Configuration pending")
    .binary_sensor("rollback_pending", TELEMETRY_CLUSTER_ID,
        entity_type=EntityType.DIAGNOSTIC,
        fallback_name="OTA validation pending")
    .sensor("confidence", TELEMETRY_CLUSTER_ID, unit=PERCENTAGE,
        state_class=SensorStateClass.MEASUREMENT,
        entity_type=EntityType.DIAGNOSTIC,
        fallback_name="Measurement confidence")
    .sensor("learning_confidence", TELEMETRY_CLUSTER_ID, unit=PERCENTAGE,
        state_class=SensorStateClass.MEASUREMENT,
        entity_type=EntityType.DIAGNOSTIC,
        fallback_name="Learning confidence")
    .sensor("raw_probe_voltage", TELEMETRY_CLUSTER_ID,
        unit=UnitOfElectricPotential.MILLIVOLT,
        device_class=SensorDeviceClass.VOLTAGE,
        state_class=SensorStateClass.MEASUREMENT,
        entity_type=EntityType.DIAGNOSTIC,
        fallback_name="Raw probe voltage")
    .sensor("measurement_noise", TELEMETRY_CLUSTER_ID,
        unit=UnitOfElectricPotential.MILLIVOLT,
        device_class=SensorDeviceClass.VOLTAGE,
        state_class=SensorStateClass.MEASUREMENT,
        entity_type=EntityType.DIAGNOSTIC,
        fallback_name="Measurement noise")
    .sensor("drying_rate", TELEMETRY_CLUSTER_ID,
        multiplier=0.01, unit="%/h",
        state_class=SensorStateClass.MEASUREMENT,
        entity_type=EntityType.DIAGNOSTIC,
        fallback_name="Drying rate")
    .sensor("sample_interval", TELEMETRY_CLUSTER_ID,
        unit=UnitOfTime.SECONDS,
        device_class=SensorDeviceClass.DURATION,
        entity_type=EntityType.DIAGNOSTIC,
        fallback_name="Sample interval")
    .sensor("seconds_since_watering", TELEMETRY_CLUSTER_ID,
        unit=UnitOfTime.SECONDS,
        device_class=SensorDeviceClass.DURATION,
        entity_type=EntityType.DIAGNOSTIC,
        initially_disabled=True,
        fallback_name="Time since watering")
    .sensor("active_dry_voltage", TELEMETRY_CLUSTER_ID,
        unit=UnitOfElectricPotential.MILLIVOLT,
        device_class=SensorDeviceClass.VOLTAGE,
        entity_type=EntityType.DIAGNOSTIC,
        fallback_name="Active dry voltage")
    .sensor("active_wet_voltage", TELEMETRY_CLUSTER_ID,
        unit=UnitOfElectricPotential.MILLIVOLT,
        device_class=SensorDeviceClass.VOLTAGE,
        entity_type=EntityType.DIAGNOSTIC,
        fallback_name="Active wet voltage")
    .sensor("learned_dry_voltage", TELEMETRY_CLUSTER_ID,
        unit=UnitOfElectricPotential.MILLIVOLT,
        device_class=SensorDeviceClass.VOLTAGE,
        entity_type=EntityType.DIAGNOSTIC,
        fallback_name="Learned dry voltage")
    .sensor("learned_wet_voltage", TELEMETRY_CLUSTER_ID,
        unit=UnitOfElectricPotential.MILLIVOLT,
        device_class=SensorDeviceClass.VOLTAGE,
        entity_type=EntityType.DIAGNOSTIC,
        fallback_name="Learned wet voltage")
    .sensor("learning_cycles", TELEMETRY_CLUSTER_ID,
        entity_type=EntityType.DIAGNOSTIC,
        fallback_name="Learning cycles")
    .sensor("applied_config_revision", TELEMETRY_CLUSTER_ID,
        entity_type=EntityType.DIAGNOSTIC,
        initially_disabled=True,
        fallback_name="Applied configuration revision")
    .sensor("ota_progress", TELEMETRY_CLUSTER_ID, unit=PERCENTAGE,
        entity_type=EntityType.DIAGNOSTIC,
        fallback_name="OTA progress")
    .sensor("active_ota_slot", TELEMETRY_CLUSTER_ID,
        entity_type=EntityType.DIAGNOSTIC,
        initially_disabled=True,
        fallback_name="Active OTA slot")
    .sensor("firmware_file_version", TELEMETRY_CLUSTER_ID,
        entity_type=EntityType.DIAGNOSTIC,
        initially_disabled=True,
        fallback_name="Firmware file version")
    .sensor("report_reason", TELEMETRY_CLUSTER_ID,
        entity_type=EntityType.DIAGNOSTIC,
        initially_disabled=True,
        fallback_name="Report reason flags")
    .add_to_registry()
)
