/*
 * lcdfill - white panel with a faint 10 px grid, in raw panel coordinates (no rotation, no offset).
 *
 * A straight-on photo of this shows the exact outline of the visible window, including the shape of
 * the rounded corners; tools/measure-bezel.py turns it into the mask used by the offline previews.
 *
 * Usage: lcdfill [-t seconds]
 */
#include "lcd.h"

int main(int argc, char **argv)
{
	int hold = 60;
	for (int i = 1; i < argc; i++)
		if (!strcmp(argv[i], "-t") && i + 1 < argc) hold = atoi(argv[++i]);

	lcd_t l;
	if (lcd_open(&l, "/dev/dri/card0", 0, 0)) return 1;
	lcd_fill(&l, 0, 0, l.w, l.h, LCD_WHITE);
	for (int x = 0; x < l.w; x += 10)
		for (int y = 0; y < l.h; y++) lcd_pixel(&l, x, y, RGB(110, 110, 110));
	for (int y = 0; y < l.h; y += 10)
		for (int x = 0; x < l.w; x++) lcd_pixel(&l, x, y, RGB(110, 110, 110));
	lcd_flush(&l);
	printf("fill %dx%d drawn, holding %d s\n", l.w, l.h, hold);
	fflush(stdout);
	sleep(hold);
	lcd_close(&l);
	return 0;
}
