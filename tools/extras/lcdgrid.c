/*
 * lcdgrid - calibration pattern in raw panel coordinates (no rotation, no offset).
 *
 * 1 px lines every 10 px (dim; every 50 px bright), 2 px coloured border at the physical panel
 * edges (left red, right green, top blue, bottom yellow) and numbered lines, so a photo shows
 * exactly which panel pixels are visible through the bezel.
 *
 * Usage: lcdgrid [-t seconds]
 */
#include "lcd.h"

int main(int argc, char **argv)
{
	int hold = 60;
	for (int i = 1; i < argc; i++)
		if (!strcmp(argv[i], "-t") && i + 1 < argc) hold = atoi(argv[++i]);

	lcd_t l;
	if (lcd_open(&l, "/dev/dri/card0", 0, 0)) return 1;
	lcd_fill(&l, 0, 0, l.w, l.h, LCD_BLACK);

	for (int x = 0; x < l.w; x += 10)
		for (int y = 0; y < l.h; y++)
			lcd_pixel(&l, x, y, x % 50 == 0 ? RGB(200, 200, 200) : RGB(70, 70, 70));
	for (int y = 0; y < l.h; y += 10)
		for (int x = 0; x < l.w; x++)
			lcd_pixel(&l, x, y, y % 50 == 0 ? RGB(200, 200, 200) : RGB(70, 70, 70));

	lcd_fill(&l, 0, 0, 2, l.h, RGB(255, 0, 0));
	lcd_fill(&l, l.w - 2, 0, 2, l.h, RGB(0, 255, 0));
	lcd_fill(&l, 0, 0, l.w, 2, RGB(0, 0, 255));
	lcd_fill(&l, 0, l.h - 2, l.w, 2, RGB(255, 255, 0));

	char buf[16];
	for (int x = 50; x < l.w; x += 50) {
		snprintf(buf, sizeof buf, "%d", x);
		lcd_text(&l, x - 4 * (int)strlen(buf), 110, buf, RGB(0, 255, 255), LCD_BLACK, 1);
	}
	for (int y = 50; y < l.h; y += 50) {
		snprintf(buf, sizeof buf, "%d", y);
		lcd_text(&l, 52, y - 8, buf, RGB(255, 0, 255), LCD_BLACK, 1);
	}
	lcd_text(&l, 12, 14, "raw", LCD_WHITE, LCD_BLACK, 1);
	lcd_text(&l, 12, l.h - 30, "bottom", LCD_WHITE, LCD_BLACK, 1);

	lcd_flush(&l);
	printf("grid %dx%d drawn, holding %d s\n", l.w, l.h, hold);
	fflush(stdout);
	sleep(hold);
	lcd_close(&l);
	return 0;
}
