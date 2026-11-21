# CrossPoint Plus

This is my personal build of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader). I added the reading and light controls I wanted on my own devices. It is a community project, not an official CrossPoint release.

## What I changed

- Ruled paper lines you can turn on and adjust while reading. Choose solid or dotted lines and change their spacing and length. The lines stay out of chapter headings.
- A quick sleep screen that appears before the rest of the sleep work finishes.
- A frontlight schedule that follows local time. Set when the light should turn off and when it should turn back on. On the Picco, the schedule controls the light by default. At a cold boot, an ON switch during scheduled-off hours turns it on; moving the switch during either schedule window overrides the light until the next schedule change. On devices using a button for the light, turning it on during scheduled-off hours keeps it on through sleep and shows a five-second reminder on wake. Turning it off during scheduled-on hours lasts until the next sleep wake, when the schedule turns it back on.

## Devices

I have tested the firmware on my own **X3/X4, X4 Pro, and Picco**. Builds for all eight device profiles currently supported by this project completed successfully; the other five still need testing on the actual devices.

| Build | Status |
| --- | --- |
| X3 / X4 | Tested |
| X4 Pro | Tested |
| Picco | Tested |
| reTerminal Sticky | Build checked; needs device testing |
| X4 Classic | Build checked; needs device testing |
| M5PaperMono | Build checked; needs device testing |
| Metalio E-Ink 4 | Build checked; needs device testing |
| EEGOREAD A4 | Build checked; needs device testing |

## Getting started

For general setup, supported formats, buttons, and installation help, start with the [CrossPoint Reader user guide](https://github.com/crosspoint-reader/crosspoint-reader/blob/develop/USER_GUIDE.md). This fork follows CrossPoint's open-source license and keeps its original project history.

Test builds are available in the repository’s [Releases](https://github.com/ImGooseI/crosspoint-reader/releases) tab.
