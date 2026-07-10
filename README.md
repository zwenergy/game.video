# game.video
A video out adapter for the Tiger game.com handheld.

## General Overview
This project taps in the digital video signals of the Tiger game.com handheld as well as the analog audio (unfortunately, no digital audio is available as the Sharp SM8521 microcontroller has an integrated DAC for audio) and breaks them out to a 10 pin IDC connector.
This connector is placed in the upper cart slot of the handheld to make a complete no-cut mod (note that it only uses the space of the empty cart slot, no connections to the actual cart connector is done here).
A small video out adapter is then connected via an IDC cable which outputs digital video and audio at 720p@60 Hz.

## Hardware Pieces
The project makes use of 3 custom PCBs.
### Video Adapter PCB

### Flex PCB

### Connector PCB

## Disclaimer
**Use the files and/or schematics to build your own board at your own risk**.
This project works fine for me, but it's a simple hobby project, so there is no liability for errors in the schematics and/or board files.
**Use at your own risk**.
