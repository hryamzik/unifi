/*
 * lcdframes - safe-area test. Draws nested 1px frames at insets 0,2,4,...,30 px, each with a
 * number label at the top-centre and left-centre, so you can see which frames the bezel hides
 * and whether the visible window is offset.
 *
 * Usage: lcdframes [-x] [-y] [-t seconds]
 */
#include "lcd.h"

int main(int argc, char **argv)
{
	int fx = 0, fy = 0, hold = 60;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-x")) fx = 1;
		else if (!strcmp(argv[i], "-y")) fy = 1;
		else if (!strcmp(argv[i], "-t") && i + 1 < argc) hold = atoi(argv[++i]);
	}

	lcd_t l;
	if (lcd_open(&l, "/dev/dri/card0", fx, fy)) return 1;
	lcd_fill(&l, 0, 0, l.w, l.h, LCD_BLACK);

	static const uint32_t col[] = {
		RGB(255, 0, 0), RGB(0, 255, 0), RGB(255, 255, 0), RGB(255, 255, 255),
	};
	/* frame n sits n*2 px in from each edge */
	for (int n = 0; n < 16; n++) {
		int in = n * 2;
		uint32_t c = col[n % 4];
		for (int x = in; x < l.w - in; x++) { lcd_pixel(&l, x, in, c); lcd_pixel(&l, x, l.h - 1 - in, c); }
		for (int y = in; y < l.h - in; y++) { lcd_pixel(&l, in, y, c); lcd_pixel(&l, l.w - 1 - in, y, c); }
	}

	/* inset labels just inside the innermost frame, one per 8 px, down the vertical centre line */
	lcd_text(&l, 40, 40,  "TOP", LCD_WHITE, LCD_BLACK, 2);
	lcd_text(&l, 24, 100, "frames", LCD_WHITE, LCD_BLACK, 2);
	lcd_text(&l, 24, 124, "step 2px", RGB(255, 255, 0), LCD_BLACK, 1);
	lcd_text(&l, 24, 140, "R=0 G=2 Y=4", RGB(180, 180, 180), LCD_BLACK, 1);
	lcd_text(&l, 24, 156, "W=6 R=8 ...", RGB(180, 180, 180), LCD_BLACK, 1);
	lcd_text(&l, 28, l.h - 60, "BOTTOM", LCD_WHITE, LCD_BLACK, 2);

	/* corner pixels: distinct per corner, to detect offset */
	lcd_fill(&l, 0, 0, 3, 3, RGB(255, 0, 0));
	lcd_fill(&l, l.w - 3, 0, 3, 3, RGB(0, 255, 0));
	lcd_fill(&l, 0, l.h - 3, 3, 3, RGB(0, 0, 255));
	lcd_fill(&l, l.w - 3, l.h - 3, 3, 3, LCD_WHITE);

	lcd_flush(&l);
	printf("drawn (flip_x=%d flip_y=%d), holding %d s\n", fx, fy, hold);
	fflush(stdout);
	sleep(hold);
	lcd_close(&l);
	return 0;
}
