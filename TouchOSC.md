# TouchOSC

[← Back to main README](README.md)

[TouchOSC](https://hexler.net/touchosc) can be used as a controller. Download the [WorkshopOSC.tosc](WorkshopOSC.tosc) project file and open it in TouchOSC. This is a very basic project that demonstrates the outputs.

With the python running on your computer make a new OSC connection in TouchOSC. If you `Browse` you should be able to see `WC OSC Bridge`. The `Send Port` is `7000` The `Receive Port` is 7001. If you press the `(i)` next to the `Receive port` you can see the ip address of the TouchOSC host. Restart the python with this address e.g. `uv run wc_osc_bridge.py --osc-send-ip 192.168.1.182` this enables communication to and from `TouchOSC` and the Workshop Computer.
