"""Serial test data sender for simulated UAVs around Domat/Ems.

This script emits one INJECT line per UAV every second to a serial port.
The generated lines are compatible with Receiver/src/main.cpp.
"""

from __future__ import annotations

import argparse
import math
import random
import time
from dataclasses import dataclass, field

try:
	import serial
except ImportError as exc:  # pragma: no cover
	raise SystemExit(
		"pyserial fehlt. Bitte installieren mit: pip install pyserial"
	) from exc


# Domat/Ems (Graubunden, CH) as default operator position
DOMAT_EMS_LAT = 46.8350
DOMAT_EMS_LON = 9.4510


@dataclass
class Drone:
	index: int
	operator_lat: float
	operator_lon: float
	max_radius_m: float
	x_m: float = 0.0
	y_m: float = 0.0
	heading_deg: float = 0.0
	speed_mps: float = 0.0
	altitude_m: int = 120
	rssi: int = -65
	next_send_ts: float = field(default_factory=time.monotonic)

	def __post_init__(self) -> None:
		angle = random.uniform(0.0, 2.0 * math.pi)
		radius = random.uniform(50.0, self.max_radius_m)
		self.x_m = radius * math.cos(angle)
		self.y_m = radius * math.sin(angle)
		self.heading_deg = random.uniform(0.0, 360.0)
		self.speed_mps = random.uniform(4.0, 25.0)
		self.altitude_m = random.randint(60, 180)
		self.rssi = random.randint(-80, -45)

	@property
	def mac(self) -> str:
		# Locally-administered unicast MAC range: 02:xx:xx:xx:xx:xx
		return f"02:00:00:00:00:{self.index:02x}"

	@property
	def uav_id(self) -> str:
		return f"UAV-{self.index:02d}"
	
	@property
	def operator_id(self) -> str:
		return f"OP-{self.index:02d}"    

	def _limit_to_radius(self) -> None:
		distance = math.hypot(self.x_m, self.y_m)
		if distance <= self.max_radius_m:
			return

		scale = self.max_radius_m / distance
		self.x_m *= scale
		self.y_m *= scale

		# Turn roughly toward center when border is reached.
		to_center_deg = math.degrees(math.atan2(-self.x_m, -self.y_m)) % 360.0
		self.heading_deg = (to_center_deg + random.uniform(-35.0, 35.0)) % 360.0

	def step(self, dt_seconds: float) -> None:
		# Mild random maneuvering and speed variance.
		self.heading_deg = (self.heading_deg + random.uniform(-10.0, 10.0)) % 360.0
		self.speed_mps = min(30.0, max(2.0, self.speed_mps + random.uniform(-1.2, 1.2)))

		heading_rad = math.radians(self.heading_deg)
		self.x_m += math.sin(heading_rad) * self.speed_mps * dt_seconds
		self.y_m += math.cos(heading_rad) * self.speed_mps * dt_seconds
		self._limit_to_radius()

		self.altitude_m = int(min(220, max(30, self.altitude_m + random.randint(-2, 2))))
		self.rssi = max(-95, min(-35, self.rssi + random.randint(-2, 2)))

	def lat_lon(self) -> tuple[float, float]:
		lat = self.operator_lat + (self.y_m / 111_320.0)
		meters_per_deg_lon = 111_320.0 * math.cos(math.radians(self.operator_lat))
		lon = self.operator_lon + (self.x_m / meters_per_deg_lon)
		return lat, lon

	def to_inject_line(self) -> str:
		lat, lon = self.lat_lon()
		heading = int(round(self.heading_deg)) % 360
		speed = int(round(self.speed_mps))
		return (
			f"INJECT {self.mac} {lat:.6f} {lon:.6f} "
			f"{self.altitude_m} {heading} {speed} {self.rssi} "
			f"{self.operator_lat:.6f} {self.operator_lon:.6f} {self.uav_id} {self.operator_id}"
		)


def parse_args() -> argparse.Namespace:
	parser = argparse.ArgumentParser(
		description=(
			"Simuliert UAV-Testdaten um Domat/Ems und sendet sie seriell als INJECT-Kommandos"
		)
	)
	parser.add_argument("--port", required=True, help="Serielle Schnittstelle, z. B. COM5")
	parser.add_argument("--baud", type=int, default=115200, help="Baudrate (Default: 115200)")
	parser.add_argument(
		"--drones", type=int, default=8, help="Anzahl simulierte Drohnen (Default: 8)"
	)
	parser.add_argument(
		"--radius",
		type=float,
		default=1500.0,
		help="Maximaler Radius um Operator in Metern (Default: 1500)",
	)
	parser.add_argument(
		"--operator-lat",
		type=float,
		default=DOMAT_EMS_LAT,
		help=f"Operator Latitude (Default: {DOMAT_EMS_LAT})",
	)
	parser.add_argument(
		"--operator-lon",
		type=float,
		default=DOMAT_EMS_LON,
		help=f"Operator Longitude (Default: {DOMAT_EMS_LON})",
	)
	parser.add_argument(
		"--seed", type=int, default=None, help="Optionaler Seed fur reproduzierbare Bewegung"
	)
	parser.add_argument(
		"--drain-rx",
		action="store_true",
		help=(
			"Liest eingehende Daten vom Receiver mit, damit dessen Serial-TX-Puffer nicht volllauft"
		),
	)
	return parser.parse_args()


def build_drones(args: argparse.Namespace) -> list[Drone]:
	return [
		Drone(
			index=i + 1,
			operator_lat=args.operator_lat,
			operator_lon=args.operator_lon,
			max_radius_m=args.radius,
			next_send_ts=time.monotonic() + random.uniform(0.0, 1.0),
		)
		for i in range(args.drones)
	]


def main() -> None:
	args = parse_args()
	if args.seed is not None:
		random.seed(args.seed)

	if args.drones <= 0:
		raise SystemExit("--drones muss > 0 sein")
	if args.radius <= 0:
		raise SystemExit("--radius muss > 0 sein")

	drones = build_drones(args)

	print(
		f"Sende {len(drones)} Drohnen auf {args.port} @ {args.baud} Baud. "
		f"Operator bei ({args.operator_lat:.6f}, {args.operator_lon:.6f}), "
		f"Radius {args.radius:.0f} m."
	)
	if args.drain_rx:
		print("RX-Drain aktiv: eingehende Receiver-Ausgaben werden mitgelesen.")
	print("Stoppen mit STRG+C")

	last_status_send = time.monotonic()

	try:
		while True:
			ser = None
			try:
				ser = serial.Serial(args.port, args.baud, timeout=1)
				if args.drain_rx:
					ser.reset_input_buffer()
				print(f"[Verbunden] {args.port}")

				while True:
					now = time.monotonic()

					if args.drain_rx:
						try:
							pending = ser.in_waiting
							if pending:
								data = ser.read(min(pending, 8192))
								if data:
									print(data.decode("ascii", errors="replace"), end="")
						except serial.SerialException:
							raise  # Weiterleiten an den aeusseren except-Block

					for drone in drones:
						if now < drone.next_send_ts:
							continue

						dt = max(0.5, now - (drone.next_send_ts - 1.0))
						drone.step(dt)

						line = drone.to_inject_line()
						ser.write((line + "\n").encode("ascii"))
						ser.flush()
						print(line)

						# Keep stable 1 Hz per drone independent of loop jitter.
						drone.next_send_ts += 1.0
						if drone.next_send_ts < now:
							drone.next_send_ts = now + 0.01

					time.sleep(0.02)


			except serial.SerialException as exc:
				print(f"\n[Verbindungsfehler] {exc}")
				print(f"[Reconnect] Warte 3 Sekunden, dann neuer Verbindungsversuch auf {args.port} ...")
			finally:
				if ser is not None and ser.is_open:
					try:
						ser.close()
					except Exception:
						pass

			# Warte vor Reconnect; KeyboardInterrupt bricht hier sauber ab.
			time.sleep(3.0)

	except KeyboardInterrupt:
		print("\nBeendet.")


if __name__ == "__main__":
	main()
