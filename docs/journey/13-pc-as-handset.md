# 13. The PC as the handset

## Where we were

Everything so far was driven by typing console commands: `mpm start frskyx`,
`mpm ch 1 1844`. That's fine on the bench, but a transmitter needs a **handset**:
something that sends the stick positions many times a second, picks the model,
starts bind. The project's plan is for the PC to be that handset, over USB, with a
real radio (the FlySky) plugged into the GPIO pins at times.

Chapter 12 made this possible: the protocol now runs from a timer interrupt, so the
main loop can spend as long as it likes on input.

## The problem

What should the PC send, and how?

## Options and the decision

**The format.**

1. **Our own commands** (`mpm ch` over the console). Text, slow to parse, one
   channel per command, and nothing anyone else speaks.
2. **A new binary struct** of our own. Fast, but every rule (when does a model
   change restart the protocol? what does the bind flag do?) would be ours to
   invent and get wrong.
3. **Multiprotocol's serial frames**, the 26–36 bytes EdgeTX sends a Multi module
   every few milliseconds: protocol, sub-protocol, RX number, option, bind/range/
   power flags, and 16 channels of 11 bits packed like SBUS.

Option 3. The format is documented in `Multiprotocol.h`, every rule already exists
in Multiprotocol's `update_serial_data()` and can be ported line by line, and the
same parser will serve every later input: PPM from the FlySky gets turned into
these frames, and a serial port on CN2 would carry them as they are.

**The transport.** The PC already reaches the CPU's memory through the USB debug
bridge (chapter 4), and the console works through a mailbox there. So the frames
go into the same mailbox: an 8-slot ring after the console rings, written by the
PC, read by the firmware. No new hardware, no new bitstream.

## How it was built

### The ring

[`console.h`](../../fw/src/console.h) grew a section at offset `0xC40`:

```c
uint32_t in_head;     /* host: frames written */
uint32_t in_tail;     /* fw: frames taken */
uint32_t in_status;   /* fw: usb / signal / paused / running / binding / bad protocol */
uint32_t in_model;    /* fw: running protocol | sub << 8 | RX << 16 */
...
struct { uint32_t len; uint8_t data[36]; } in[8];
```

The same rule as the console: each counter is written by one side only, so no
locks are needed between PC and firmware. The PC writes a frame into slot
`head % 8`, then the new `head`, and reads the status words back, all in **one**
bridge transaction ([`multi_link.py`](../../tools/multi_link.py)). A
`_Static_assert` in [`input.c`](../../fw/src/input.c) pins the offsets so the C
struct and the Python can't drift apart silently.

### The parser

`mpm_serial_frame()` in [`mpm_core.cpp`](../../fw/src/mpm/mpm_core.cpp) is
Multiprotocol's `update_serial_data()` with the AVR parts removed:

- flags (range, autobind, power) and the option apply at once;
- a different protocol, sub-protocol or RX number is a **new model**: the protocol
  restarts (in bind mode if the bind flag is set);
- the bind flag going 1 restarts in bind mode; going 0 asks the protocol to end
  bind;
- the 16 channels unpack from 22 bytes (11 bits each, least significant bit
  first, like SBUS).

It runs with interrupts off for the few microseconds it takes, so a protocol
callback never sees half a frame's channels.

### Link loss

What if the PC crashes, or the cable comes out? Multiprotocol's answer: no frame
for 70 ms means **no signal**, and it stops transmitting, so the receiver goes to
its failsafe. Here the same: `input.c` pauses the scheduler (`mpm_pause`), and
the next frame resumes it. The timeout is settable (`input timeout MS`) because
over the USB/IP tunnel a frame takes a few hundred milliseconds.

### Who is in control

Frames take control when they arrive, as when a handset is plugged into a
module. `mpm start` and `mpm stop` on the console take it back. Simple to explain,
and you never have to wonder which one is driving.

## How we knew it worked

[`input_check.py`](../../tools/input_check.py) runs eight checks on the board:
a frame starts FrSky X, 16 channel values arrive exactly, bind on and off, frames
stop and the callbacks stop, frames resume and so do the callbacks, a frame for
AFHDS2A switches protocol, and `mpm stop` takes control back.

The first run failed four of the eight, and the firmware was fine. The test read
the status **in the same transaction** that wrote the frame, so it always saw the
state from *before* that frame. Over the tunnel each check also took seconds, long
enough for the 3 s timeout to pause the protocol between checks. The log showed
it: the bind flag appeared in the status one frame late. The test was fixed to
behave like a real handset (a thread that keeps sending frames) and to wait until
the firmware had taken two new frames before checking. 8/8.

Streaming frames from `multi_link.py` made no callback late (worst 2 µs), and the
option (−40) and channel values (1600 µs → 1184, −50% → 614) arrived exactly.

## A gamepad as the sticks

Typing channel values isn't flying. [`gamepad.py`](../../tools/gamepad.py) reads a
gamepad through pygame (SDL underneath, so the same code works on Linux, macOS and
Windows, and common pads get standard input names). It's in two parts on purpose:

- **`Mapping`** is pure: a dict of inputs in (`{"rightx": 0.5, "a": True}`), 16
  channel values out. Deadband, expo, end points, inverting, and buttons as
  momentary or latching switches. Because it touches no hardware, it has unit
  tests ([`test_gamepad.py`](../../tools/test_gamepad.py)).
- **`Gamepad`** just reads the pad into that dict.

Two safety habits from real radios came along: nothing is sent until the
throttle stick is low, and if the pad disconnects, sending stops, so the firmware's
link-loss pause puts the receiver in failsafe.

To test without a real gamepad, a virtual Xbox 360 pad was created with Linux
`uinput`, holding known stick positions and tapping A once. The first run read
seven of eight channels right; the latching switch on A missed its tap. Over the
tunnel the pad was sampled once per frame (about once a second), and the 0.3 s tap
fell between two samples. The fix counts SDL's button-press **events** instead of
comparing samples, so no tap is lost at any frame rate. Second run: 8/8.

## A radio on the trainer port (PPM)

Sometimes the handset will be a real radio: a FlySky, which can't speak Multi
serial, only PPM out of its trainer port. PPM is a pulse at the start of each
channel slot; the time from one pulse to the next is the channel's value (1000 to
2000 µs), and a longer gap marks the start of a frame.

Measuring microseconds is a job for the FPGA, not the CPU, so
[`ppm_in`](../../hw/ip/ppm_in/ppm_in.v) times the intervals in 0.5 µs steps and
keeps the last complete frame for the firmware. The rules (what counts as a
glitch, what counts as the sync, how many channels make a frame) are copied from
Multiprotocol's PPM interrupt, and the firmware converts widths to channel values
with Multiprotocol's own integer maths, so a radio behaves the same as on a
Multiprotocol module.

The radio was at home. So the block got a **built-in PPM generator** that the
firmware can switch in place of the pin. That isn't only for this test: it means
the decoder, the firmware and the link-loss pause can be checked on any board,
any time, without a cable. The testbench drives the pin itself (both polarities,
a glitch, a frame with too few channels, 16 channels at the extremes) and the
generator (11 checks).

Two things the testbench taught: a frame is only complete when the **next**
sync edge arrives (the sync gap has to end before you know it was one), and the
first edge after power-up can't be trusted, since nothing says how long the line
was idle before it. Multiprotocol behaves the same way; the first version of the
test didn't.

## What it led to

The PC can now fly the board, from a gamepad. Next on this path: PPM from the FlySky's trainer
port on a GPIO pin, turned into the same frames; and, for ExpressLRS later, CRSF,
which is what EdgeTX speaks to an ELRS module.

## The thinking move

**Reuse the protocol the ecosystem already speaks, and port its rules instead of
inventing them.** Every edge case (what a model change means, what happens to
bind, what happens with no signal) was already decided and field-tested by
Multiprotocol. And when a test fails, **check the test against the timing of the
real system** before suspecting the code: a status read one step too early looks
exactly like a bug.
