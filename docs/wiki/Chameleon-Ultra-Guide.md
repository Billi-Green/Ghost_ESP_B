# Chameleon Ultra Integration Guide

The Ghost ESP now includes comprehensive support for the **Chameleon Ultra**, a powerful 13.56MHz NFC/RFID research tool. This integration allows you to control your Chameleon Ultra remotely via Bluetooth and perform advanced card analysis and attacks.

## 🔗 Quick Start

### Prerequisites
- **Chameleon Ultra** device with Bluetooth enabled
- **SD Card** in Ghost ESP (for saving scan results and dumps)
- Both devices powered on and in range

### Basic Connection
```bash
chameleon connect      # Scan and connect to Chameleon Ultra
chameleon status       # Verify connection and device info
chameleon battery      # Check battery level
```

## 📡 Core Operations

### Device Management
```bash
chameleon connect         # Connect to Chameleon Ultra
chameleon disconnect      # Disconnect from device
chameleon status          # Show connection and device info
chameleon battery         # Display battery level
chameleon firmware        # Show firmware version
chameleon devicemode      # Display current mode (Reader/Emulator)
```

### Mode Switching
```bash
chameleon reader          # Switch to reader mode (for scanning cards)
chameleon emulator        # Switch to emulator mode (for card simulation)
```

## 🎯 HF (13.56MHz) Operations

### Basic Scanning
```bash
chameleon scanhf          # Scan for HF cards (MIFARE, NTAG, ISO14443)
chameleon savehf          # Save scan results with auto-generated filename
chameleon savehf mycard   # Save with custom filename
```

**Supported Card Types:**
- MIFARE Classic (1K, 4K)
- MIFARE Ultralight
- NTAG213/215/216
- ISO14443 Type A/B cards

### Advanced MIFARE Classic Analysis
```bash
chameleon readhf          # Comprehensive automated analysis:
                         #   - Default key testing (12 common keys)
                         #   - Automatic Darkside attacks
                         #   - Automatic Nested attacks
                         #   - Complete data dumping

chameleon savedump        # Save complete analysis results
```

**The `readhf` command performs a 3-phase automated analysis:**

1. **Phase 1: Default Key Testing**
   - Tests 12 common MIFARE keys on all 16 sectors
   - Attempts both Key A and Key B for each sector
   - Records successful authentications

2. **Phase 2: Darkside Attack**
   - Automatically runs Darkside attack on sectors with failed authentication
   - Collects nonces for offline key recovery
   - Saves attack data to individual files per sector

3. **Phase 3: Nested Attack**
   - Uses any discovered keys to attack remaining locked sectors
   - Leverages key relationships between sectors
   - Maximizes key recovery potential

4. **Phase 4: Data Reading**
   - Reads all accessible blocks using discovered keys
   - Re-authenticates as needed for each sector
   - Creates comprehensive dump with authentication summary

## 🏷️ NTAG Operations

### NTAG Detection and Analysis
```bash
chameleon ntagdetect      # Intelligent NTAG type detection
chameleon ntagdump        # Analyze card structure and read accessible data
chameleon saventralag     # Save analysis results
```

**Special Features:**
- **Protected Card Support**: Handles password-protected NTAG cards gracefully
- **Multi-Method Detection**: Uses GET_VERSION and memory structure analysis
- **Professional Reports**: Creates detailed forensic analysis documents

**Example NTAG215 Analysis Output:**
```
NTAG Card Analysis Report
========================
Card Type: NTAG215 (Password Protected)
UID (7 bytes): 04 8A EE 1B C3 2A 81

Analysis Results:
================
- Card detected as NTAG type (likely NTAG215)
- Standard commands return authentication error (0x60)
- Card appears to be password protected or locked
- Access requires proper authentication credentials

Recommendations:
===============
1. This card requires a 4-byte password for access
2. Try common default passwords: 00000000, FFFFFFFF
3. Check if this is a custom application with known passwords
4. Consider using specialized NTAG cracking tools
```

## 🔓 Advanced MIFARE Classic Attacks

### Manual Attack Commands
```bash
# MIFARE Detection
chameleon mfdetect        # Detect MIFARE Classic support
chameleon mfprng          # Test PRNG weakness

# Darkside Attack (Key Recovery)
chameleon darkside 4 A    # Attack block 4, Key A
chameleon darkside 8 B    # Attack block 8, Key B
chameleon savedarkside    # Save collected nonces

# Nested Attack (Known key -> Unknown key)
chameleon nested 0 A FFFFFFFFFFFF 4 A    # Use known key on block 0 to attack block 4
chameleon savenested      # Save nested attack data
```

**Attack Parameters:**
- **Block**: 0-63 (sector trailer blocks: 3, 7, 11, 15, etc.)
- **Key Type**: A or B
- **Known Key**: 12-character hex (e.g., FFFFFFFFFFFF)

### Default Keys Tested
The system automatically tests these common MIFARE keys:
```
FFFFFFFFFFFF  (Factory default)
A0A1A2A3A4A5  (Transport key)
D3F7D3F7D3F7  (MAD key)
000000000000  (Blank key)
B0B1B2B3B4B5  (Alternative transport)
4D3A99C351DD  (Hotel cards)
1A982C7E459A  (Campus cards)
AABBCCDDEEFF  (Test key)
714C5C886E97  (Generic)
587EE5F9350F  (Conference badges)
A0478CC39091  (Access control)
533CB6C723F6  (Parking systems)
```

## 📊 LF (125KHz) Operations

### Low Frequency Scanning
```bash
chameleon scanlf          # Scan for EM410X tags
chameleon scanhidprox     # Scan for HID Proximity cards  
chameleon scanlfall       # Try both EM410X and HID Prox
chameleon savelf          # Save LF scan results
chameleon readlf          # Read LF card data
```

**Supported LF Formats:**
- EM410X (64-bit ID cards)
- HID Proximity (125KHz access cards)
- Various proprietary formats

## 🎰 Slot Management

The Chameleon Ultra has 8 card slots for emulation:

```bash
chameleon activeslot      # Show current active slot (1-8)
chameleon setslot 3       # Switch to slot 3
chameleon slotinfo 5      # Show info for slot 5
```

**Note**: Slot numbers are displayed as 1-8 for user convenience (device uses 0-7 internally).

## 💾 File Management

### Save Locations
All files are saved to `/mnt/ghostesp/chameleon/` on the SD card:

```
/mnt/ghostesp/chameleon/
├── hf_scan_04AB1234_20241216_143022.txt      # HF scan results
├── mifare_dump_04AB1234_20241216_143055.txt  # Complete MIFARE dumps
├── darkside_04AB1234_sector_02_20241216.txt  # Darkside attack data
├── nested_04AB1234_block_04_20241216.txt     # Nested attack data  
├── ntag_protected_048AEE1B_20241216.txt      # NTAG analysis
└── lf_scan_em410x_20241216_143101.txt        # LF scan results
```

### Filename Formats
- **Auto-generated**: Include UID, timestamp, and card type
- **Custom**: Use optional filename parameter: `chameleon savehf mycard`
- **Timestamped**: All files include creation date/time

## 🛠️ Troubleshooting

### Connection Issues
```bash
# Check available memory
chameleon status          # Memory info included in status

# Reconnection sequence
chameleon disconnect
# Wait 5 seconds
chameleon connect
```

### Memory Requirements
- **Minimum**: 20KB free heap for stable operation
- **Recommended**: 40KB+ for complex operations
- System automatically checks memory before initialization

### Common Issues

**"Not connected to Chameleon Ultra"**
- Ensure device is powered on and in range
- Try reconnecting: `chameleon disconnect` then `chameleon connect`

**"Insufficient memory"**
- Restart Ghost ESP: `reboot`
- Close other applications consuming memory

**"Card detection failed"**
- Ensure card is properly positioned
- Try switching to reader mode: `chameleon reader`
- Check battery level: `chameleon battery`

**"Authentication required (Status 0x60)"**
- Card is password-protected (normal for NTAG)
- Use analysis commands for professional documentation
- Consider specialized tools for password recovery

### Status Codes
- **0x00**: Success (tag found)
- **0x68**: General success
- **0x60**: Authentication required
- **0x66**: Wrong device mode
- **0x41**: No LF tag found

## 🚀 Pro Tips

### Efficient Workflow
1. **Initial Setup**:
   ```bash
   chameleon connect
   chameleon battery      # Ensure sufficient power
   chameleon reader       # Set appropriate mode
   ```

2. **Card Analysis**:
   ```bash
   chameleon scanhf       # Quick scan first
   chameleon readhf       # Full automated analysis
   chameleon savedump     # Save comprehensive results
   ```

3. **NTAG Cards**:
   ```bash
   chameleon ntagdetect   # Identify type and protection
   chameleon ntagdump     # Analyze structure
   chameleon saventralag  # Document findings
   ```

### Security Research Best Practices
- Always document your findings with save commands
- Use custom filenames for organized research projects
- Review generated reports for detailed technical information
- Leverage automated attacks in `readhf` for comprehensive analysis

### Integration with External Tools
- **Darkside data**: Compatible with `mfcuk` and `mfoc`
- **Nested data**: Works with `libnfc-mfcuk` and similar tools
- **Card dumps**: Standard format for analysis software
- **Professional reports**: Suitable for security assessments

## 📚 Additional Resources

- [Chameleon Ultra Official Documentation](https://github.com/RfidResearchGroup/ChameleonUltra)
- [MIFARE Classic Attacks Reference](https://github.com/RfidResearchGroup/ChameleonUltra/wiki/protocol)
- [NTAG Password Protection Guide](https://www.nxp.com/docs/en/application-note/AN11495.pdf)

---

**Note**: This integration is designed for security research and educational purposes. Always ensure you have proper authorization before testing with any cards or systems.
