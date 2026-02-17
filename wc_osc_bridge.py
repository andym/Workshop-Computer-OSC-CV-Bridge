#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = [
#     "pyserial",
#     "python-osc",
#     "zeroconf",
# ]
# ///
"""
wc_osc_bridge.py

Bridges a Workshop Computer (running custom OSC bridge firmware) to any
OSC controller via USB CDC.

Uses a compact binary protocol over USB CDC.
The firmware runs ProcessSample at 48kHz, so outputs are smooth even at
high update rates.

Two directions:
  OSC → WC:  OSC client sends messages → this script → binary USB → WC outputs
  WC → OSC:  WC inputs → binary USB → this script → OSC → OSC server

Binary protocol:
  Host→Device (10 bytes): 0xC0, flags, int16[4] (little-endian, -2048..2047)
  Device→Host (16 bytes): 0xC1, flags, int16[2] CV, int16[2] audio, int16[3] knobs

Channel → Workshop Computer output mapping:
  /ch/1  →  Audio Out 1  (SPI DAC, 12-bit, 48kHz — best for LFO/continuous CV)
  /ch/2  →  Audio Out 2  (SPI DAC, 12-bit, 48kHz)
  /ch/3  →  CV Out 1     (PWM, 11-bit, MIDI-calibrated — best for V/oct)
  /ch/4  →  CV Out 2     (PWM, 11-bit)
  /pulse/1 → Pulse Out 1  (GPIO, digital — gates/triggers, threshold > 0V)
  /pulse/2 → Pulse Out 2  (GPIO, digital)

Workshop Computer input → OSC mapping:
  Audio In 1  →  /ch/1
  Audio In 2  →  /ch/2
  CV In 1     →  /ch/3
  CV In 2     →  /ch/4
  Main knob   →  /knob/main  (0.0-6.0V)
  X knob      →  /knob/x     (0.0-6.0V)
  Y knob      →  /knob/y     (0.0-6.0V)
  Switch      →  /switch     (0=down, 1=middle, 2=up)
  Pulse In 1  →  /pulse/1    (1.0 or 0.0)
  Pulse In 2  →  /pulse/2    (1.0 or 0.0)

All values are ComputerCard native range: -2048 to +2047
(approx -6V to +6V, 12V span). Voltage conversion is done here in Python.

Usage:
  uv run wc_osc_bridge.py --port /dev/tty.usbmodem*

OSC setup:
  - Send OSC messages to port 7000 (this script receives them)
  - Listen on port 7001 for WC input values (this script sends them)
  - Output channels: /ch/1 .. /ch/4, /pulse/1 .. /pulse/2
  - Input channels: /ch/1 .. /ch/4, /knob/main, /knob/x, /knob/y, /switch, /pulse/1, /pulse/2
"""

import argparse
import socket
import struct
import threading
import time
import sys
import serial
from pythonosc import udp_client, dispatcher, osc_server
from zeroconf import ServiceInfo, Zeroconf


# ---------------------------------------------------------------------------
# Protocol constants
# ---------------------------------------------------------------------------

SYNC_HOST_TO_DEVICE = 0xC0
SYNC_DEVICE_TO_HOST = 0xC1
OUTPUT_PACKET_SIZE = 10  # host → device
INPUT_PACKET_SIZE = 16   # device → host

# ComputerCard native range: -2048 to 2047
# Maps to approximately -6V to +6V (12V range)
NATIVE_MIN = -2048
NATIVE_MAX = 2047
VOLTAGE_RANGE = 12.0  # total voltage span


def volts_to_native(volts: float) -> int:
    """Convert voltage (-6V to +6V) to ComputerCard native int16 (-2048 to 2047)."""
    native = int(volts / VOLTAGE_RANGE * (NATIVE_MAX - NATIVE_MIN + 1))
    return max(NATIVE_MIN, min(NATIVE_MAX, native))


def native_to_volts(native: int) -> float:
    """Convert ComputerCard native int16 (-2048 to 2047) to voltage."""
    return native * VOLTAGE_RANGE / (NATIVE_MAX - NATIVE_MIN + 1)


# ---------------------------------------------------------------------------
# Serial helpers
# ---------------------------------------------------------------------------

def get_local_ip():
    """Get the local network IP address (not 127.0.0.1)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except Exception:
        return "127.0.0.1"
    finally:
        s.close()


def find_wc_port():
    """Try to auto-detect a Workshop Computer serial port."""
    import serial.tools.list_ports
    candidates = []
    for port in serial.tools.list_ports.comports():
        dev = port.device.lower()
        if "usbmodem" in dev or "acm" in dev:
            candidates.append(port)
            print(f"  candidate: {port.device}  ({port.description})")
    if len(candidates) == 1:
        return candidates[0].device
    return None


def open_serial(port):
    """Open USB CDC connection to Workshop Computer."""
    ser = serial.Serial(port, timeout=0.01)
    ser.read(ser.in_waiting or 1)  # flush
    return ser


# ---------------------------------------------------------------------------
# OSC → Workshop Computer (binary USB)
# ---------------------------------------------------------------------------

class OutputBridge:
    NUM_CV = 4

    def __init__(self, ser, verbose=False):
        self.ser = ser
        self.verbose = verbose

        # Latest native values from OSC (1-indexed, [0] unused)
        self.latest = [0] * (self.NUM_CV + 1)
        self.pulse = [False, False]
        self.lock = threading.Lock()

    def osc_handler(self, address, *args):
        """Called by OSC dispatcher for /ch/* and /pulse/*. Sends immediately."""
        if not args:
            return
        try:
            volts = float(args[0])
        except (ValueError, TypeError):
            return

        parts = address.strip("/").split("/")
        try:
            num = int(parts[-1])
        except (ValueError, IndexError):
            return

        volts = max(-6.0, min(6.0, volts))
        native = volts_to_native(volts)

        with self.lock:
            if parts[0] == "ch" and 1 <= num <= self.NUM_CV:
                self.latest[num] = native
            elif parts[0] == "pulse" and 1 <= num <= 2:
                self.pulse[num - 1] = native > 0
            else:
                return

            flags = (0x01 if self.pulse[0] else 0) | (0x02 if self.pulse[1] else 0)
            vals = self.latest[1:self.NUM_CV + 1]
            packet = struct.pack('<BB4h',
                                 SYNC_HOST_TO_DEVICE, flags,
                                 vals[0], vals[1], vals[2], vals[3])
            self.ser.write(packet)

        if self.verbose:
            print(f"  [OSC in] {address} {args[0]}")


# ---------------------------------------------------------------------------
# Workshop Computer → OSC (binary USB → OSC)
# ---------------------------------------------------------------------------

def reader_thread(ser, osc_client, verbose=False, threshold=0.005):
    """
    Reads binary packets from Workshop Computer and sends as OSC.
    Only sends a message when the value has changed by more than threshold.

    Device→Host packet (16 bytes):
      0xC1, flags, int16 cv1, int16 cv2, int16 audio1, int16 audio2,
      int16 knob_main, int16 knob_x, int16 knob_y

    flags: bit 0 = pulse1, bit 1 = pulse2, bits 2-3 = switch (0/1/2)
    """
    buf = bytearray()
    last = {}  # address → last sent value

    def send_if_changed(address, value):
        prev = last.get(address)
        if prev is None or abs(value - prev) > threshold:
            osc_client.send_message(address, value)
            last[address] = value
            if verbose:
                print(f"  [OSC out] {address} {value:.4f}")

    while True:
        try:
            data = ser.read(ser.in_waiting or 1)
            if not data:
                continue
            buf.extend(data)

            # Scan for complete packets
            while len(buf) >= INPUT_PACKET_SIZE:
                # Find sync byte
                try:
                    idx = buf.index(SYNC_DEVICE_TO_HOST)
                except ValueError:
                    buf.clear()
                    break

                # Discard bytes before sync
                if idx > 0:
                    del buf[:idx]

                if len(buf) < INPUT_PACKET_SIZE:
                    break

                # Extract packet
                pkt = bytes(buf[:INPUT_PACKET_SIZE])
                del buf[:INPUT_PACKET_SIZE]

                flags = pkt[1]
                cv1, cv2, audio1, audio2, knob_main, knob_x, knob_y = \
                    struct.unpack_from('<7h', pkt, 2)

                pulse1 = bool(flags & 0x01)
                pulse2 = bool(flags & 0x02)
                switch_pos = (flags >> 2) & 0x03

                # Send inputs as OSC voltages (top-to-bottom: audio, CV)
                send_if_changed("/ch/1", native_to_volts(audio1))
                send_if_changed("/ch/2", native_to_volts(audio2))
                send_if_changed("/ch/3", native_to_volts(cv1))
                send_if_changed("/ch/4", native_to_volts(cv2))

                # Send knobs as 0.0-6.0V
                send_if_changed("/knob/main", knob_main * 6.0 / 4095.0)
                send_if_changed("/knob/x", knob_x * 6.0 / 4095.0)
                send_if_changed("/knob/y", knob_y * 6.0 / 4095.0)

                # Send switch and pulses (always send — discrete values)
                send_if_changed("/switch", float(switch_pos))
                send_if_changed("/pulse/1", 1.0 if pulse1 else 0.0)
                send_if_changed("/pulse/2", 1.0 if pulse2 else 0.0)

        except serial.SerialException:
            print("Serial connection lost!")
            sys.exit(1)
        except Exception as e:
            print(f"Reader error: {e}")
            time.sleep(0.1)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Bridge Workshop Computer (custom firmware) ↔ OSC"
    )
    parser.add_argument(
        "--port", "-p",
        help="Serial port for Workshop Computer (auto-detected if omitted)"
    )
    parser.add_argument(
        "--osc-send-port", type=int, default=7001,
        help="UDP port to SEND OSC (WC inputs) (default: 7001)"
    )
    parser.add_argument(
        "--osc-recv-port", type=int, default=7000,
        help="UDP port to RECEIVE OSC (WC outputs) (default: 7000)"
    )
    parser.add_argument(
        "--osc-send-ip", default="127.0.0.1",
        help="IP to send OSC to (default: 127.0.0.1)"
    )
    parser.add_argument(
        "--localhost", action="store_true",
        help="Listen on 127.0.0.1 only (default: 0.0.0.0, all interfaces)"
    )
    parser.add_argument(
        "--threshold", "-t", type=float, default=0.005,
        help="Change threshold for OSC output — suppress messages below this (default: 0.005)"
    )
    parser.add_argument(
        "--show-traffic", "-s", nargs="?", const="all",
        choices=["all", "in", "out"],
        help="Show OSC traffic: all (default), in (from network), out (to network)"
    )
    args = parser.parse_args()

    # --- Find serial port ---
    port = args.port
    if not port:
        print("Scanning for Workshop Computer...")
        port = find_wc_port()
        if not port:
            print("Could not auto-detect Workshop Computer. Available ports:")
            import serial.tools.list_ports
            for p in serial.tools.list_ports.comports():
                print(f"  {p.device}  -  {p.description}  ({p.manufacturer})")
            print("\nSpecify with --port /dev/tty.usbmodemXXXX")
            sys.exit(1)

    print(f"Opening serial: {port}")
    ser = open_serial(port)

    # --- Set up the output bridge ---
    show_in = args.show_traffic in ("all", "in")
    show_out = args.show_traffic in ("all", "out")
    bridge = OutputBridge(ser, verbose=show_in)

    # --- Set up OSC ---
    listen_ip = "127.0.0.1" if args.localhost else "0.0.0.0"
    local_ip = get_local_ip()

    print(f"OSC send → {args.osc_send_ip}:{args.osc_send_port}  (WC inputs → OSC)")
    client = udp_client.SimpleUDPClient(args.osc_send_ip, args.osc_send_port)

    print(f"OSC recv ← {listen_ip}:{args.osc_recv_port}  (OSC → WC outputs)")
    disp = dispatcher.Dispatcher()
    disp.map("/ch/*", bridge.osc_handler)
    disp.map("/pulse/*", bridge.osc_handler)

    osc_srv = osc_server.ThreadingOSCUDPServer(
        (listen_ip, args.osc_recv_port), disp
    )

    # --- Advertise via Zeroconf (mDNS) for TouchOSC discovery ---
    zc = None
    zc_info = None
    if not args.localhost:
        try:
            zc_info = ServiceInfo(
                "_osc._udp.local.",
                "WC OSC Bridge._osc._udp.local.",
                addresses=[socket.inet_aton(local_ip)],
                port=args.osc_recv_port,
            )
            zc = Zeroconf()
            zc.register_service(zc_info)
            print(f"Zeroconf: advertising as 'WC OSC Bridge' on {local_ip}:{args.osc_recv_port}")
        except Exception as e:
            print(f"Zeroconf: could not advertise ({e})")

    # --- Start reader thread ---
    threading.Thread(
        target=reader_thread,
        args=(ser, client, show_out, args.threshold),
        daemon=True,
    ).start()

    print(f"\nBridge running (send-on-receive, out={OUTPUT_PACKET_SIZE}B in={INPUT_PACKET_SIZE}B). Ctrl+C to quit.\n")
    print(f"  Local IP: {local_ip}")
    print(f"  Send to bridge:      port {args.osc_recv_port}  (/ch/1-4, /pulse/1-2)")
    print(f"  Receive from bridge: port {args.osc_send_port}  (/ch/1-4, /knob/*, /switch, /pulse/1-2)")
    print()

    try:
        osc_srv.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down...")
        # Zero all outputs on exit
        packet = struct.pack('<BB4h', SYNC_HOST_TO_DEVICE, 0x00, 0, 0, 0, 0)
        try:
            ser.write(packet)
        except serial.SerialException:
            pass
        ser.close()
        osc_srv.shutdown()
        if zc:
            zc.unregister_service(zc_info)
            zc.close()
        print("Done.")


if __name__ == "__main__":
    main()
