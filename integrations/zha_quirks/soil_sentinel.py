"""ZHA quirk for longhornx10 Soil Sentinel telemetry."""

from typing import Final

import zigpy.types as t
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


class SoilSentinelMode(t.enum8):
    """Adaptive operating mode reported by the firmware."""

    Stable = 0
    Drying = 1
    Near_dry = 2
    Critical = 3
    Watering_capture = 4
    Recently_watered = 5
    Conservation = 6
    Survival = 7


class SoilSentinelEventFlags(t.bitmap16):
    """Reasons that caused the most recent Zigbee state report."""

    Threshold = 1 << 0
    Watering = 1 << 1
    Fault = 1 << 2
    Heartbeat = 1 << 3
    Battery = 1 << 4
    Manual = 1 << 5


class SoilSentinelTelemetryCluster(CustomCluster):
    """Compact telemetry cluster unpacked into local ZHA attributes."""

    cluster_id = 0xFC00
    ep_attribute = "soil_sentinel_telemetry"

    TELEMETRY_SCHEMA_VERSION = 1
    TELEMETRY_PAYLOAD_LENGTH = 21

    class AttributeDefs(BaseAttributeDefs):
        """Device and locally-derived telemetry attributes."""

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

    def _update_attribute(self, attrid: int, value) -> None:
        """Decode the firmware's single low-power telemetry frame."""

        super()._update_attribute(attrid, value)
        if attrid != self.AttributeDefs.telemetry.id:
            return

        payload = bytes(value)
        if (
            len(payload) != self.TELEMETRY_PAYLOAD_LENGTH
            or payload[0] != self.TELEMETRY_SCHEMA_VERSION
        ):
            return

        self._update_attribute(
            self.AttributeDefs.operating_mode.id, SoilSentinelMode(payload[1])
        )
        self._update_attribute(
            self.AttributeDefs.report_reason.id,
            SoilSentinelEventFlags(int.from_bytes(payload[2:4], "little")),
        )
        self._update_attribute(self.AttributeDefs.sensor_fault.id, bool(payload[4]))
        self._update_attribute(self.AttributeDefs.confidence.id, payload[5])
        self._update_attribute(
            self.AttributeDefs.raw_probe_voltage.id,
            int.from_bytes(payload[6:8], "little"),
        )
        self._update_attribute(
            self.AttributeDefs.measurement_noise.id,
            int.from_bytes(payload[8:10], "little"),
        )
        self._update_attribute(
            self.AttributeDefs.drying_rate.id,
            int.from_bytes(payload[10:12], "little", signed=True),
        )
        self._update_attribute(
            self.AttributeDefs.sample_interval.id,
            int.from_bytes(payload[12:16], "little"),
        )
        self._update_attribute(
            self.AttributeDefs.seconds_since_watering.id,
            int.from_bytes(payload[16:20], "little"),
        )
        self._update_attribute(self.AttributeDefs.battery_present.id, bool(payload[20]))


(
    QuirkBuilder("longhornx10", "Soil Sentinel")
    .replaces(SoilSentinelTelemetryCluster)
    .enum(
        SoilSentinelTelemetryCluster.AttributeDefs.operating_mode.name,
        SoilSentinelMode,
        SoilSentinelTelemetryCluster.cluster_id,
        entity_platform=EntityPlatform.SENSOR,
        entity_type=EntityType.DIAGNOSTIC,
        fallback_name="Operating mode",
    )
    .binary_sensor(
        SoilSentinelTelemetryCluster.AttributeDefs.sensor_fault.name,
        SoilSentinelTelemetryCluster.cluster_id,
        device_class=BinarySensorDeviceClass.PROBLEM,
        entity_type=EntityType.DIAGNOSTIC,
        fallback_name="Sensor fault",
    )
    .sensor(
        SoilSentinelTelemetryCluster.AttributeDefs.confidence.name,
        SoilSentinelTelemetryCluster.cluster_id,
        unit=PERCENTAGE,
        state_class=SensorStateClass.MEASUREMENT,
        entity_type=EntityType.DIAGNOSTIC,
        fallback_name="Measurement confidence",
    )
    .sensor(
        SoilSentinelTelemetryCluster.AttributeDefs.drying_rate.name,
        SoilSentinelTelemetryCluster.cluster_id,
        multiplier=0.01,
        unit="%/h",
        state_class=SensorStateClass.MEASUREMENT,
        entity_type=EntityType.DIAGNOSTIC,
        fallback_name="Drying rate",
    )
    .sensor(
        SoilSentinelTelemetryCluster.AttributeDefs.sample_interval.name,
        SoilSentinelTelemetryCluster.cluster_id,
        unit=UnitOfTime.SECONDS,
        device_class=SensorDeviceClass.DURATION,
        entity_type=EntityType.DIAGNOSTIC,
        fallback_name="Sample interval",
    )
    .sensor(
        SoilSentinelTelemetryCluster.AttributeDefs.seconds_since_watering.name,
        SoilSentinelTelemetryCluster.cluster_id,
        unit=UnitOfTime.SECONDS,
        device_class=SensorDeviceClass.DURATION,
        entity_type=EntityType.DIAGNOSTIC,
        fallback_name="Time since watering",
    )
    .sensor(
        SoilSentinelTelemetryCluster.AttributeDefs.raw_probe_voltage.name,
        SoilSentinelTelemetryCluster.cluster_id,
        unit=UnitOfElectricPotential.MILLIVOLT,
        device_class=SensorDeviceClass.VOLTAGE,
        state_class=SensorStateClass.MEASUREMENT,
        entity_type=EntityType.DIAGNOSTIC,
        fallback_name="Raw probe voltage",
    )
    .sensor(
        SoilSentinelTelemetryCluster.AttributeDefs.measurement_noise.name,
        SoilSentinelTelemetryCluster.cluster_id,
        unit=UnitOfElectricPotential.MILLIVOLT,
        device_class=SensorDeviceClass.VOLTAGE,
        state_class=SensorStateClass.MEASUREMENT,
        entity_type=EntityType.DIAGNOSTIC,
        fallback_name="Measurement noise",
    )
    .sensor(
        SoilSentinelTelemetryCluster.AttributeDefs.report_reason.name,
        SoilSentinelTelemetryCluster.cluster_id,
        entity_type=EntityType.DIAGNOSTIC,
        initially_disabled=True,
        fallback_name="Report reason flags",
    )
    .binary_sensor(
        SoilSentinelTelemetryCluster.AttributeDefs.battery_present.name,
        SoilSentinelTelemetryCluster.cluster_id,
        entity_type=EntityType.DIAGNOSTIC,
        initially_disabled=True,
        fallback_name="Battery present",
    )
    .add_to_registry()
)
