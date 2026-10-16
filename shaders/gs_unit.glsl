// PS2 unit scale: the GS treats alpha 0x80 as 1.0, so alpha is scaled by 255/128.
float ps2AScale(float a) { return a * (255.0 / 128.0); }
