# VCV Rack 2

[← Back to main README](README.md)

I've been using this with [VCV Rack 2](https://vcvrack.com/Rack)

Install the [cvOSCcv module](https://library.vcvrack.com/trowaSoft/cvOSCcv) and set the "Send Frequency" to 1000Hz.

Press `config` and then `enable`.

Patch some CV from your virtual rack to the inputs and it'll come out of the CV on your workshop computer. Patch the outputs of cvOSCcv into your virtual rack and control that from your workshop.

You'll need to press the config button on cvOSCcv to change some of the paths to e.g. `/knob/main` on the output side, or `/pulse/1/` on the input side.
