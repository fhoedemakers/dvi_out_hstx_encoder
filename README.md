# GPIO Pin Assignment

How to assign GPIO Pin layouts:
To configure the alternate GPIO layout for the TMDS lanes, you need to update the assignments in the `main()` function where the `hstx_ctrl_hw->bit` array is configured. Specifically, you need to map the TMDS lanes to the new GPIO pairs as follows:

- **D0+ = GPIO18, D0- = GPIO19**
- **D1+ = GPIO16, D1- = GPIO17**
- **D2+ = GPIO12, D2- = GPIO13**

Here’s how you can modify the code to reflect the new GPIO layout:

```c
// ...existing code...

// Updated GPIO layout for TMDS lanes
static const int lane_to_output_bit[3] = {6, 4, 0}; // D0 = 6, D1 = 4, D2 = 0

// Assign clock pair to two neighboring pins:
hstx_ctrl_hw->bit[2] = HSTX_CTRL_BIT0_CLK_BITS;
hstx_ctrl_hw->bit[3] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;

for (uint lane = 0; lane < 3; ++lane) {
    int bit = lane_to_output_bit[lane];
    uint32_t lane_data_sel_bits =
        (lane * 10    ) << HSTX_CTRL_BIT0_SEL_P_LSB |
        (lane * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB;
    hstx_ctrl_hw->bit[bit    ] = lane_data_sel_bits;
    hstx_ctrl_hw->bit[bit + 1] = lane_data_sel_bits | HSTX_CTRL_BIT0_INV_BITS;
}

// Configure GPIOs for HSTX function
for (int i = 12; i <= 19; ++i) {
    gpio_set_function(i, 0); // HSTX
}

// ...existing code...
```

### Explanation of Changes:
1. **Updated `lane_to_output_bit` Array**:
   - The `lane_to_output_bit` array maps TMDS lanes to the new GPIO pairs:
     - `D0` (GPIO18, GPIO19) corresponds to bit 6.
     - `D1` (GPIO16, GPIO17) corresponds to bit 4.
     - `D2` (GPIO12, GPIO13) corresponds to bit 0.

2. **Clock Pair Assignment**:
   - The clock pair (CK+ and CK-) remains on GPIO14 and GPIO15, so no changes are needed for the clock configuration.

3. **GPIO Configuration**:
   - The `gpio_set_function()` loop remains unchanged, as it already configures GPIOs 12 through 19 for the HSTX function.

This modification ensures that the TMDS lanes are correctly mapped to the new GPIO layout.

The `lane_to_output_bit` array defines the mapping between TMDS lanes (D0, D1, D2) and the **HSTX output bits** that correspond to specific GPIO pins. Here's how it works:

### HSTX Output Bits and GPIO Pins
The HSTX hardware outputs data on 8 "output bits" (0 through 7), which are mapped to specific GPIO pins. The mapping is fixed as follows:

- **HSTX Output Bit 0 → GPIO12**
- **HSTX Output Bit 1 → GPIO13**
- **HSTX Output Bit 2 → GPIO14**
- **HSTX Output Bit 3 → GPIO15**
- **HSTX Output Bit 4 → GPIO16**
- **HSTX Output Bit 5 → GPIO17**
- **HSTX Output Bit 6 → GPIO18**
- **HSTX Output Bit 7 → GPIO19**

### `lane_to_output_bit` Array
The `lane_to_output_bit` array specifies which HSTX output bits are used for each TMDS lane:

- **Index 0**: TMDS lane D0 (data lane 0)
- **Index 1**: TMDS lane D1 (data lane 1)
- **Index 2**: TMDS lane D2 (data lane 2)

For the array `{6, 4, 0}`:
- **D0 (Index 0)** is assigned to HSTX output bit 6 → GPIO18 (D0+) and GPIO19 (D0-).
- **D1 (Index 1)** is assigned to HSTX output bit 4 → GPIO16 (D1+) and GPIO17 (D1-).
- **D2 (Index 2)** is assigned to HSTX output bit 0 → GPIO12 (D2+) and GPIO13 (D2-).

### How This Relates to GPIO Pins
The `lane_to_output_bit` array determines which GPIO pins are used for each TMDS lane by referencing the fixed mapping between HSTX output bits and GPIO pins. For example:
- When `lane_to_output_bit[0] = 6`, it means TMDS lane D0 will use HSTX output bit 6, which corresponds to GPIO18 and GPIO19.
- Similarly, `lane_to_output_bit[1] = 4` means TMDS lane D1 will use HSTX output bit 4, which corresponds to GPIO16 and GPIO17.
- Finally, `lane_to_output_bit[2] = 0` means TMDS lane D2 will use HSTX output bit 0, which corresponds to GPIO12 and GPIO13.

### Summary
The `lane_to_output_bit` array is a way to map TMDS lanes (D0, D1, D2) to specific GPIO pins by referencing the HSTX output bit-to-GPIO mapping. This allows you to configure the TMDS lanes to match your desired GPIO layout.


# Using 2bytes per pixel in the framebuffer

If the framebuffer uses **RGB565** instead of **RGB332**, the size of each pixel increases from **1 byte** (RGB332) to **2 bytes** (RGB565). This change affects the calculation of the size of a scanline and the DMA configuration for transferring pixel data.

### Changes Required for RGB565:

#### 1. **Scanline Size in Framebuffer**
For **RGB565**, each pixel is 2 bytes. The size of a scanline becomes:
```c
MODE_H_ACTIVE_PIXELS * 2
```
For `MODE_H_ACTIVE_PIXELS = 640`:
```c
Scanline size = 640 * 2 = 1280 bytes
```

#### 2. **DMA Transfer Count**
The `transfer_count` in the DMA configuration must be updated to reflect the new pixel size. Since the DMA transfer count is in terms of **32-bit words (4 bytes)**, the number of words per scanline is:
```c
MODE_H_ACTIVE_PIXELS * 2 / sizeof(uint32_t)
```
For `MODE_H_ACTIVE_PIXELS = 640`:
```c
Transfer count = (640 * 2) / 4 = 320 words
```

Update the DMA configuration in the `dma_irq_handler`:
```c
ch->transfer_count = (MODE_H_ACTIVE_PIXELS * 2) / sizeof(uint32_t);
```

#### 3. **Framebuffer Data Format**
Ensure the framebuffer (`framebuf`) is updated to store **RGB565** data. Each pixel should be represented as a 16-bit value:
- **RGB565 Format**:
  - 5 bits for red
  - 6 bits for green
  - 5 bits for blue

If you are generating the framebuffer data, use the `colour_rgb565` function to encode RGB values into the RGB565 format:
```c
static __force_inline uint16_t colour_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint16_t)r & 0xf8) >> 3 | ((uint16_t)g & 0xfc) << 3 | ((uint16_t)b & 0xf8) << 8;
}
```

#### 4. **Update TMDS Encoder Configuration**
If the TMDS encoder is configured for **RGB332**, you need to update it for **RGB565**. Modify the `hstx_ctrl_hw->expand_tmds` configuration to reflect the new bit depth per pixel.

For example:
```c
hstx_ctrl_hw->expand_tmds =
    5  << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |
    0  << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB   |
    5  << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |
    29 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB   |
    6  << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |
    26 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;
```
Here, the `*_NBITS_LSB` fields are updated to reflect the bit depth of each color channel in RGB565 (5 bits for red, 6 bits for green, and 5 bits for blue).

#### 5. **Adjust Command Lists**
If any command lists (e.g., `vactive_line`) depend on the pixel format, ensure they are updated to handle the new data size and format.

For example, in `vactive_line`:
```c
HSTX_CMD_TMDS | MODE_H_ACTIVE_PIXELS
```
This remains the same because `MODE_H_ACTIVE_PIXELS` refers to the number of pixels, not bytes.

---

### Summary of Changes:
1. Update the scanline size to **1280 bytes**.
2. Update the DMA `transfer_count` to **320 words**.
3. Ensure the framebuffer stores **RGB565** data (16 bits per pixel).
4. Reconfigure the TMDS encoder for **RGB565**.
5. Verify and adjust any command lists or related configurations.

These changes will ensure the system correctly handles the **RGB565** format for the framebuffer and outputs the appropriate video signal.

# 16 bit colors, anders dan hierboven

To adjust the TMDS encoder for **16-bit colors (RGB565)**, you need to modify the configuration of the TMDS encoder to handle the increased bit depth per pixel. The current configuration is set up for **8-bit colors (RGB332)**, so adjustments are required to account for the **16 bits per pixel** in RGB565.

---

### Steps to Adjust the TMDS Encoder for 16-bit Colors:

#### 1. **Update the TMDS Encoder Configuration**
The TMDS encoder is configured using the `hstx_ctrl_hw->expand_tmds` register. This register defines how the TMDS lanes expand the pixel data. For RGB565, you need to update the number of bits per color channel (5 bits for red, 6 bits for green, and 5 bits for blue).

Modify the `expand_tmds` configuration as follows:
```c
hstx_ctrl_hw->expand_tmds =
    5  << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |  // 5 bits for red
    0  << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB   |  // No rotation for red
    6  << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |  // 6 bits for green
    29 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB   |  // Rotation for green
    5  << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |  // 5 bits for blue
    26 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;     // Rotation for blue
```

#### Explanation:
- **`*_NBITS_LSB`**: Specifies the number of bits for each color channel.
  - Red: 5 bits
  - Green: 6 bits
  - Blue: 5 bits
- **`*_ROT_LSB`**: Specifies the rotation for each color channel. This is used to align the bits correctly for the TMDS lanes. The values (e.g., `29`, `26`) depend on the hardware's TMDS encoder implementation.

---

#### 2. **Update the Framebuffer Format**
Ensure the framebuffer (`framebuf`) is updated to store **16-bit RGB565** data. Each pixel should now be represented as a 16-bit value. If you are generating the framebuffer data, use the `colour_rgb565` function to encode RGB values into the RGB565 format:
```c
static __force_inline uint16_t colour_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint16_t)r & 0xf8) >> 3 |  // Top 5 bits of red
           ((uint16_t)g & 0xfc) << 3 |  // Top 6 bits of green
           ((uint16_t)b & 0xf8) << 8;   // Top 5 bits of blue
}
```

---

#### 3. **Adjust DMA Transfer Count**
The DMA transfer count must be updated to reflect the new pixel size (16 bits = 2 bytes per pixel). Update the `transfer_count` in the `dma_irq_handler`:
```c
ch->transfer_count = (MODE_H_ACTIVE_PIXELS * 2) / sizeof(uint32_t);
```

Explanation:
- `MODE_H_ACTIVE_PIXELS * 2`: Total bytes in a scanline (2 bytes per pixel).
- `sizeof(uint32_t)`: The DMA transfer count is in terms of 32-bit words (4 bytes).

---

#### 4. **Update Command Lists**
If the command lists (e.g., `vactive_line`) depend on the pixel format, ensure they are updated to handle the new data size. For example, in `vactive_line`:
```c
static uint32_t vactive_line[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_TMDS       | MODE_H_ACTIVE_PIXELS  // No change needed here
};
```
The `MODE_H_ACTIVE_PIXELS` value remains the same because it refers to the number of pixels, not bytes.

---

#### 5. **Verify TMDS Clock and Bandwidth**
Switching to 16-bit colors doubles the data bandwidth compared to 8-bit colors. Ensure that:
- The TMDS clock is fast enough to handle the increased data rate.
- The HSTX FIFO and DMA can sustain the higher data throughput.

---

### Summary of Changes:
1. Update `hstx_ctrl_hw->expand_tmds` to handle 16-bit RGB565 (5 bits red, 6 bits green, 5 bits blue).
2. Ensure the framebuffer stores 16-bit RGB565 data.
3. Adjust the DMA `transfer_count` to account for 2 bytes per pixel.
4. Verify and update command lists if necessary.
5. Ensure the TMDS clock and hardware can handle the increased bandwidth.

These changes will configure the TMDS encoder to correctly process and output 16-bit RGB565 color data.
