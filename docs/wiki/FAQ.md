# Frequently Asked questions

1. What are the default network credentials?
    - SSID: `GhostNet`
    - Pass: `GhostNet`
1. What are the default credentials to the web interface?
    - User: `GhostNet`
    - Pass: `GhostNet`
1. How do I access the web interface?
    - Just type in `ghostesp.local` or `192.168.4.1` into your browsers address bar after connecting to the GhostNet AP
1. Why dont the default credentials work to log into the web interface?
    - The web credentials are identical to the SSID, and network password you've set. If you've changed these values your web credentials will also change.
1. How do I flash my board?
    - See the [installation documentation](https://github.com/jaylikesbunda/Ghost_ESP/wiki/Installation#installation-guide)
    - The web flasher can be found at <https://flasher.ghostesp.net/>
1. Can I upload custom evil portal html over Serial/from my flipper zero?
    - Unfortunately customer evil portal html can on be set from an sd card directly connected to the Ghost ESP board.
1. My board isn't currently supported. Will you add support?
    - Unfortunately due to the limited amount of contributors new boards are unlikely to be supported at this time unless there is significant demand. If you wish you can always compile a custom build configuration for your board and open a pull request.
1. Why does the does my connection to the Ghost ESP AP drop when issuing wifi commands?
    - This is a limitation of the ESP32 - The wifi chip can only be in AP mode or Client mode but not both simultaneously.
1. Im not seeing any output when connecting via serial
    - The ghost esp firmware is silent unless a command is running. Try running the help command to check if youve made a proper connection.
1. Why wont my SD card work?
    - "generic" builds of the software do not include SD card support by default.
    - If you arent using a generic build ensure your SD card is formatted as fat32
