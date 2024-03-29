#include <hidef.h>			/* common defines and macros */
#include "derivative.h"			/* derivative-specific definitions */


#define COLS (20)
#define ROWS (4)

#define SCAN_TICKS (200u) // 1ms on time per column
#define DEAD_TICKS (5u) // 27us all channels off

#define DEBOUNCE_INTERVAL (374u) // ignores fluctuations for 2ms

char note; // the currently playing note index
char playing; // boolean currently playing
unsigned char song[COLS]; // stores the notes in the song using their periods in timer counts
char keypressed; // whether a key was just pressed
char key; // the key that was last pressed
char cursorX; // the x position of the cursor, starting at 0 (top down coordinate system)
char cursorY; // the y position of the cursor, starting at 0
unsigned char rti_count; // the number of rti interrupts that have occurred since the last note increment
unsigned int debounce_expire; // the time when inputs should stop being ignored
unsigned char keypad_col; // the column on the keypad that is currently pulled low

/*
PWM @ M = 1 / N = 184
C4 - 261.63 Hz / 132 cm = 0.003882 s = 3.882 ms = 3882 us / # Ticks = 253.63 => 254
D4 - 293.66 Hz / 117 cm = 0.003405 s = 3.405 ms = 3405 us / # Ticks = 222.0652 => 222
E4 - 329.63 Hz / 105 cm = 0.003034 s = 3.034 ms = 3034 us / # Ticks = 197.8696 => 198
F4 - 349.23 Hz / 98.8 cm = 0.002863 s = 2.863 ms = 2863 us / # Ticks = 187
G4 - 392.00 Hz / 88.0 cm = 0.002551 s = 2.551 ms = 2551 us / # Ticks = 166.3696 => 166
A4 - 440.00 Hz / 78.4 cm = 0.002273 s = 2.273 ms = 2273 us / # Ticks = 148.2391 => 148
*/
unsigned char pwmTable[] = { 187, 198, 222, 254 };

// setup keypad constants
#define RIGHT '8'
#define LEFT '2'
#define UP '6'
#define DOWN '4'
#define ENTER '5'
#define CLEAR '7'
#define PLAY_PAUSE '1'

// delay for t microseconds
void DelayuSec(int t)
{
	if(t == 0) {
		return;
	}

	__asm {
			ldx t		; get number of usec to delay
			; Main loop is 24 cycles, or 1 usec
		loop:
			psha		; 2 E cycles
			pula		; 3 E cycles
			psha		; 2 E cycles
			pula		; 3 E cycles
			psha		; 2 E cycles
			pula		; 3 E cycles
			psha		; 2 E cycles
			pula		; 3 E cycles
			nop			; 1 E cycle
			dbne x,loop ; 3 E cycles
	}
}

/* This function writes a nibble to the LCD.	Input parameters:
	 n contains the nibble to be written (in the lower 4 bits)
	 rs indicates instruction or data (rs=0 for inst, rs=1 for data)
	 t is the time to delay after sending (units of 1 us)
Assumes these connections:
PT0:PT3 - connect to LCD pins 8:14 (DB4:DB7)
PT4 - connect to LCD pin 6 (E)
PT5 - connect to LCD pin 4 (RS)
*/
void writeNibbleToLCD(char n, char rs, int t)
{
	rs <<= 5; // get rs bit into the bit 5 position
	PTT_PTT0 = 1; // set E to 1
	PTM = rs|(0x0f & n); // output the nibble and RS bit
	DelayuSec(1); // keep E pulse high a little while
	PTT_PTT0 = 0; // set E to 0
	DelayuSec(t);
}

// writes a byte to the LCD, sending the high nibble first, then the low nibble
void writeByteToLCD(char b, char rs, int t)
{
	writeNibbleToLCD(b >> 4, rs, 50);
	writeNibbleToLCD(b, rs, 50);
	DelayuSec(t);
}

// clears the display and reset the cursor to the home position
void clearLCD()
{
	writeByteToLCD(0x01, 0, 2000);
}

// prints the string to the display
void printLCD(char mystr[])
{
	int i = 0;
	clearLCD();

	while (*mystr) {
		if (i++ >= 80) {
			break;
		}
		writeByteToLCD(*(mystr++), 1, 50);
	}
}

// initializes the LCD to be ready to accept strings
void InitializeLCD(void)
{
	int i;
	for (i=0; i<100; i++) { // delay 100ms to allow LCD powerup
		DelayuSec(1000);
	}
	// The first parameter in each call is the nibble to be sent,
	// the second parameter is the rs value (rs=0 indicates an instruction),
	// and the third parameter is the time to delay after sending (in units of us).
	writeNibbleToLCD(0x03, 0, 5000); // delay at least 4 ms = 4000 us
	writeNibbleToLCD(0x03, 0, 5000);
	writeNibbleToLCD(0x03, 0, 5000);
	writeNibbleToLCD(0x02, 0, 5000);
	// The first parameter in each call is the byte to be sent (as two nibbles),
	// the second parameter is the rs value (rs=0 indicates an instruction),
	// and the 3rd parameter is the time to delay after sending both nibbles (usec).
	// These commands are all fast (~40 us) except for "clear display" (2 ms)
	writeByteToLCD(0x28, 0, 50); // 2 lines, 5x8 dots
	writeByteToLCD(0x0c, 0, 50); // display on, no cursor, no blinking
	writeByteToLCD(0x14, 0, 50); // shift cursor right
	writeByteToLCD(0x01, 0, 2000); // clear display and cursor home
}

/*
The keypad needs to cycle through the pins in the order 3,1,5 which equates to PT5,PT7,PT3
*/

// setup timers and interrupts for handling keypad input
void setupKeypad(void)
{
	TSCR1 = 0x80; // enable timer and disable fast flag clear
	TSCR2 = 0x87; // enable overflow interrupt, set prescaler to 128, 5.33us per tick, overflow occurs at 349.5ms

	// PT7, PT5, PT3 are output compare
	TIOS = 0xA8; // %10101000

	// set PT 7,5,3 high
	PTT |= 0xA8; // %10101000

	// set PT5 to go low
	TC5 = TCNT + SCAN_TICKS;
	TCTL1 = 0x08; // %00001000
	keypad_col = 3; // normally column 3 comes before 5

	// enable interrupts on PT7, PT5, PT3
	TIE = 0xA8; // %10101000
	TFLG1 = 0xff; // clear all timer flags
}

// setup PT1 for PWM to control the speaker
void setupPWM(void)
{
	// prescaler of 1, divider of 184
	PWMPRCLK = 0x00;
	PWMSCLA = 184;
	// select clock SA for PT1
	PWMCLK = 0x02; // %00000010
	PWMPOL = 0x02;
	PWME = 0x00;
	MODRR = 0x02;
}

// setup the RTI interrupt for advancing the current note
void setupRTI(void)
{
	RTICTL = 0x17; // set divider to 4*2^10, freq of 1953.125 Hz
	CRGINT = 0x80; // enable RTI interrupt
}

// rti interrupt
void interrupt VectorNumber_Vrti rti_isr(void)
{
	CRGFLG = 0x80;

	// the number of rti counts is controlled by the analog reading of the potentiometer
	if (rti_count++ >= ATDDR0L) {
		rti_count = 0;
		note = (note + 1) % COLS; // advance the song by one note
		if (song[note] == 0) { // if no note is set for this position, disable pwm
			PWME = 0x00;
		}
		else {
			PWME = 0x02;
			// set the period to the period of the note, and the duty cycle to half the period
			PWMPER1 = song[note];
			PWMDTY1 = song[note]/2;
		}
	}
}

// keypad control ISRs

// the columns are 3,1,5 which equate to PT5,PT7,PT3
// the pins for the rows on the keypad are 2,7,6, which equate to PT6,PT2,PT4

// When the output compare timer overflows, set the debounce to expire immediately.
// This prevents the debounce system from locking the user out.
void interrupt VectorNumber_Vtimovf ovf_isr(void)
{
	TFLG2 = 0x80; // clear overflow flag
	debounce_expire = 0;
}

// handles checking for a keypress
void handle_key(unsigned char pressed, unsigned char key_code)
{
	if (pressed && key != key_code) { // if the key is depressed and was not already pressed
		keypressed = 1; // flag a key as having been pressed
		key = key_code; // flag which key was pressed
		debounce_expire = TCNT + DEBOUNCE_INTERVAL; // set the debounce expire time
	}
	else if (!pressed && key == key_code) {   // if the key is not depressed and previously was
		key = 0; // flag that no key is pressed
		debounce_expire = TCNT + DEBOUNCE_INTERVAL;
	}
}

// output compare interrupt on PT5, which is the first column of the keypad
void interrupt VectorNumber_Vtimch5 oc5_isr(void)
{
	// clear flag
	TFLG1 = 0x20; // %00100000
	if (keypad_col == 3) { // PT3 just went high and PT5 just went low
		TCTL1 = 0x8C; // %10001100 // set PT5 to go high, PT7 to go low
		TCTL2 = 0x00;
		TC5 = TC5 + SCAN_TICKS;
		TC7 = TC5 + DEAD_TICKS;
		keypad_col = 5;

		// until the debounce interval is over we ignore all input
		if (TCNT > debounce_expire) {
			// keys are active low
			handle_key(!PTIT_PTIT6, '1');
			handle_key(!PTIT_PTIT2, '4');
			handle_key(!PTIT_PTIT4, '7');
		}
	}
}

// output compare interrupt on PT7, which is the second column of the keypad
void interrupt VectorNumber_Vtimch7 oc7_isr(void)
{
	// clear flag
	TFLG1 = 0x80; // %10000000
	if (keypad_col == 5) { // PT5 just went high and PT7 just went low
		TCTL1 = 0xC0; // %11000000 // set PT7 to go high, PT3 to go low
		TCTL2 = 0x80; // %10000000
		TC7 = TC7 + SCAN_TICKS;
		TC3 = TC7 + DEAD_TICKS;
		keypad_col = 7;

		if (TCNT > debounce_expire) {
			handle_key(!PTIT_PTIT6, '2');
			handle_key(!PTIT_PTIT2, '5');
			handle_key(!PTIT_PTIT4, '8');
		}
	}
}

// output compare interrupt on PT3, which is the third column of the keypad
void interrupt VectorNumber_Vtimch3 oc3_isr(void)
{
	// clear flag
	TFLG1 = 0x08; // %00001000
	if (keypad_col == 7) { // PT7 just went high and PT3 just went low
		TCTL1 = 0x08; // %00001000 // set PT3 to go high, PT5 to go low
		TCTL2 = 0xC0; // %11000000
		TC3 = TC3 + SCAN_TICKS;
		TC5 = TC3 + DEAD_TICKS;
		keypad_col = 3;

		if (TCNT > debounce_expire) {
			handle_key(!PTIT_PTIT6, '3');
			handle_key(!PTIT_PTIT2, '6');
			handle_key(!PTIT_PTIT4, '9');
		}
	}
}

// toggles between playing and paused states
void playOrPause()
{
	if (playing) {
		playing = 0;
		CRGINT = 0x00; // disable RTI
		PWME = 0x00; // disable PWM
	}
	else {
		playing = 1;
		CRGINT = 0x80; // enable RTI
		PWME = 0x02; // enable PWM
	}
}

// removes all the notes from the song
void clearSong()
{
	char i;
	for (i = 0; i < COLS; i++) {
		song[i] = 0;
	}
}

// move the cursor in the direction specified by the key
void moveCursor(char key)
{
	if (key == UP && cursorY > 0) {
		cursorY--;
	}
	else if (key == DOWN && cursorY < ROWS - 1) {
		cursorY++;
	}
	else if (key == RIGHT && cursorX < COLS - 1) {
		cursorX++;
	}
	else if (key == LEFT && cursorX > 0) {
		cursorX--;
	}
}

// toggle whether there is a note at the current cursor position
void setOrClearNote()
{
	// check the period of the note to determine if there is one at the current position
	if (song[cursorX] == pwmTable[cursorY]) {
		// if there is a note at this position, clear it
		song[cursorX] = 0;
	}
	else {
		// otherwise put a note here
		song[cursorX] = pwmTable[cursorY];
	}
}

// redraws the LCD
void redraw()
{
	char x, y;

	clearLCD(); // clear the lcd

	// loop over every position in the screen
	for (y = 0; y < ROWS; y += 2) {
		for (x = 0; x < COLS; x++) {
			if (x == cursorX && y == cursorY) { // if the cursor is at this position
				if (song[x] == pwmTable[y]) { // if there is a note at this position
					writeByteToLCD('+', 1, 50);
				}
				else { // just the cursor
					writeByteToLCD('|', 1, 50);
				}
			}
			else {
				if (song[x] == pwmTable[y]) { // if there is a note at this position
					writeByteToLCD('-', 1, 50);
				}
				else { // nothing here
					writeByteToLCD(' ', 1, 50);
				}
			}
		}

		// the screen memory sequentially goes to row 0,2,1,3 so y needs to follow this same pattern
		if (y == 2) {
			y = -1;
		}
	}
}

// setup the analog to digital converter for AN02
void setupADC(void)
{
	ATDCTL2 = 0xC0; // enable ATD and fast flag clear
	ATDCTL3 = 0x08; // set ATD for 1 channel conversion
	ATDCTL4 = 0x85; // set ATD for 2 MHz,2 sample clks,8 bits
	ATDCTL5 = 0xA2; // right justif, continuous conv, use AN02
	// A/D results appear in ATDDR0L
}

void main(void)
{
	// PT 7,5,3,1,0 are outputs
	DDRT = 0xAB; // %10101011
	// PM 5,3,2,1,0 are outputs
	// PM4 is an input because it is being driven by PT1
	DDRM = 0x2F; // %00101111

	InitializeLCD();

	playing = 1; // start in playing mode
	note = 0; // start at the first note
	rti_count = 0; // initialize rti count

	// initialize other global variables
	debounce_expire = 0;
	keypressed = 0;
	key = 0;
	cursorX = 0;
	cursorY = 0;

	// setup interrupts and timers
	clearSong();

	setupPWM();

	setupADC();

	setupKeypad();

	setupRTI();

	redraw();

	EnableInterrupts;

	for(;;) {
		// wait until a key is pressed
		if (keypressed) {
			keypressed = 0; // clear the key pressed flag
			if (key == UP || key == DOWN || key == RIGHT || key == LEFT) { // if a directional key was pressed, move the cursor
				moveCursor(key);
				redraw();
			}
			else if (key == ENTER) { // toggle a note at the current position
				setOrClearNote();
				redraw();
			}
			else if (key == PLAY_PAUSE) { // play or pause the song
				playOrPause();
			}
			else if (key == CLEAR) { // clear the song
				clearSong();
				redraw();
			}
		}
		_FEED_COP();
	}
}