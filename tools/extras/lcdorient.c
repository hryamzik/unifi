/*
 * lcdorient - orientation/text test for the UTR-LR LCD.
 * Draws asymmetric text so mirroring, flipping and 180-degree rotation are all distinguishable.
 *
 * Usage: lcdorient [-x] [-y] [-t seconds]
 *   -x  mirror horizontally   -y  mirror vertically
 */
#include "lcd.h"

int main(int argc, char **argv)
{
	int fx = 0, fy = 0, hold = 30;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-x")) fx = 1;
		else if (!strcmp(argv[i], "-y")) fy = 1;
		else if (!strcmp(argv[i], "-t") && i + 1 < argc) hold = atoi(argv[++i]);
	}

	lcd_t l;
	if (lcd_open(&l, "/dev/dri/card0", fx, fy)) return 1;

	lcd_fill(&l, 0, 0, l.w, l.h, RGB(0, 0, 64));

	/* header, top-left anchored */
	lcd_text(&l, 0, 0,  "UTR-LR TOP ^^", LCD_WHITE, RGB(200, 0, 0), 1);
	lcd_text(&l, 0, 24, "1234567890",    RGB(255, 255, 0), RGB(0, 0, 64), 1);
	lcd_text(&l, 0, 40, "abcdefghijk",   RGB(0, 255, 255), RGB(0, 0, 64), 1);
	/* big, clearly asymmetric glyphs */
	lcd_text(&l, 8, 72,  "F", LCD_WHITE, RGB(0, 0, 64), 4);
	lcd_text(&l, 56, 72, "R", RGB(0, 255, 0), RGB(0, 0, 64), 4);
	lcd_text(&l, 8, 140, "G", RGB(255, 128, 0), RGB(0, 0, 64), 4);
	lcd_text(&l, 56, 140, "7", RGB(255, 0, 255), RGB(0, 0, 64), 4);
	/* footer, bottom-left anchored */
	lcd_text(&l, 0, l.h - 16, "BOTTOM vv  <L", LCD_WHITE, RGB(0, 120, 0), 1);

	/* frame + corner markers */
	for (int x = 0; x < l.w; x++) { lcd_pixel(&l, x, 0, LCD_WHITE); lcd_pixel(&l, x, l.h - 1, LCD_WHITE); }
	for (int y = 0; y < l.h; y++) { lcd_pixel(&l, 0, y, LCD_WHITE); lcd_pixel(&l, l.w - 1, y, LCD_WHITE); }
	lcd_fill(&l, l.w - 12, 0, 12, 12, RGB(0, 255, 0));

	lcd_flush(&l);
	printf("drawn (flip_x=%d flip_y=%d), holding %d s\n", fx, fy, hold);
	fflush(stdout);
	sleep(hold);
	lcd_close(&l);
	return 0;
}
