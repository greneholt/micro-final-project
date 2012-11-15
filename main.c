#define COLS (20)
#define ROWS (4)

#define SCAN_TICKS (37453u) // 100ms on time per column
#define DEAD_TICKS (374u) // 1ms all channels off

char note;
char playing; // boolean currently playing
char song[COLS]; // stores note periods in timer counts
char keypressed;
char key;
char cursorX;
char cursorY;

void DelayuSec(int t) {
	if(t == 0) return;

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

/* This function writes a nibble to the LCD.  Input parameters:
	 n contains the nibble to be written (in the lower 4 bits)
	 rs indicates instruction or data (rs=0 for inst, rs=1 for data)
	 t is the time to delay after sending (units of 1 us)
Assumes these connections:
PT0:PT3 - connect to LCD pins 8:14 (DB4:DB7)
PT4 - connect to LCD pin 6 (E)
PT5 - connect to LCD pin 4 (RS)
*/
void writeNibbleToLCD(char n, char rs, int t) {
	rs <<= 5;				  // get rs bit into the bit 5 position
	PTT = rs|0x10|(0x0f & n);  // output the nibble with E=1
	DelayuSec(1);			  // keep E pulse high a little while
	PTT &= ~0x10;			  // make E go to 0
	DelayuSec(t);
}

// writes a byte to the LCD, sending the high nibble first, then the low nibble
void writeByteToLCD(char b, char rs, int t) {
	writeNibbleToLCD(b >> 4, rs, 50);
	writeNibbleToLCD(b, rs, 50);
	DelayuSec(t);
}

void clearLCD() {
	writeByteToLCD(0x01, 0, 2000);  // clear display and cursor home
}

void printLCD(char mystr[]) {
	int i = 0;
	clearLCD();

	while (*mystr) {
		if (i++ >= 80) break;
		writeByteToLCD(*(mystr++), 1, 50);
	}
}

void InitializeLCD(void) {
	int i;
	for (i=0; i<100; i++) { // delay 100ms to allow LCD powerup
		DelayuSec(1000);
	}
	// The first parameter in each call is the nibble to be sent,
	// the second parameter is the rs value (rs=0 indicates an instruction),
	// and the third parameter is the time to delay after sending (in units of us).
	writeNibbleToLCD(0x03, 0, 5000);   // delay at least 4 ms = 4000 us
	writeNibbleToLCD(0x03, 0, 5000);
	writeNibbleToLCD(0x03, 0, 5000);
	writeNibbleToLCD(0x02, 0, 5000);
	// The first parameter in each call is the byte to be sent (as two nibbles),
	// the second parameter is the rs value (rs=0 indicates an instruction),
	// and the 3rd parameter is the time to delay after sending both nibbles (usec).
	// These commands are all fast (~40 us) except for "clear display" (2 ms)
	writeByteToLCD(0x28, 0, 50);	// 2 lines, 5x8 dots
	writeByteToLCD(0x0c, 0, 50);   // display on, no cursor, no blinking
	writeByteToLCD(0x14, 0, 50);	// shift cursor right
	writeByteToLCD(0x01, 0, 2000);  // clear display and cursor home
}

/*
The keypad needs to cycle through the pins in the order 3,1,5 which equates to PT5,PT7,PT3
*/

void setupKeypad(void) {
	TSCR1 = 0x90; // enable timer and fast flag clear
	TSCR2 = 0x06; // disable overflow interrupt, set prescaler to 64, 2.67us per tick, overflow occurs at 174.8ms
	TFLG1 = 0xff; // clear all timer flags

	// PT7, PT5, PT3 are output compare, PT6, PT4, PT2 are input capture
	TIOS = 0xA8; // %10101000
	TIE = 0xFC; // %11111100

	// PT6, PT4, PT2 need capture on falling edge as they are grounded by button pushes
	TCTL3 = 0x22; // %00100010
	TCTL4 = 0x20; // $00100000

	PTT &= ~0xFC; // set PT 7-2 low

	// set PT5 to go high
	TC5 = TCNT + SCAN_TICKS;
	TCTL1 = 0x0C; // %00001100
}

void setupPWM(void) {

}

void main() {
	// PT 7,5,3,1,0 are outputs
	DDRT = 0xAB; // %10101011
	// PM 5,3,2,1,0 are outputs
	// PM4 is an input because it is being driven by PT1
	DDRM = 0x2F; // %00101111

	InitializeLCD();

	playing = 1;
	note = 0;

	keypressed = 0;
	cursorX = 0;
	cursorY = 0;

	set PT5 to go high
	TC5 = TCNT + scan interval

	set PT1 to go high
	TC1 = TCNT + song[note];

	setupPWM();

	setupKeypad();

	loop {
		if (keypressed) {
			keypressed = 0;
			if (key == UP || key == DOWN || key == RIGHT || key == LEFT) {
				moveCursor(key);
				redraw();
			}
			else if (key == ENTER) {
				setNote();
				redraw();
			}
			else if (key == PLAY_PAUSE) {
				playOrPause();
			}
			else if (key == CLEAR) {
				clearSong();
				redraw();
			}
		}
	}
}

// RTI (song advance)
isr RTI {
	note = (note + 1) % songlength;
}

// speaker control
isr PT1 output compare {
	if (PT1 low) {
		set PT1 to go high
	}
	else {
		set PT2 to go low
	}
	TC1 = TC1 + song[note];
}

// keypad control ISRs
void interrupt VectorNumber_Vtimch5 oc5_isr(void) {
	if (PTT_PTT5) { // PT3 just went low and PT5 just went high
		TCTL1 = 0xC8; // %11001000 // set PT5 to go low, PT7 to go high
		TC5 = TC5 + SCAN_TICKS;
		TC7 = TC5 + DEAD_TICKS;
	}
}

void interrupt VectorNumber_Vtimch7 oc7_isr(void) {
	if (PTT_PTT7) { // PT5 just went low and PT7 just went high
		TCTL1 = 0x80; // %10000000 // set PT7 to go low, PT3 to go high
		TCTL2 = 0xC0; // %11000000
		TC7 = TC7 + SCAN_TICKS;
		TC6 = TC7 + DEAD_TICKS;
	}
}

void interrupt VectorNumber_Vtimch3 oc3_isr(void) {
	if (PTT_PTT3) { // PT7 just went low and PT3 just went high
		TCTL1 = 0x0C; // %00001100 // set PT3 to go low, PT5 to go high
		TCTL2 = 0x80; // %10000000
		TC3 = TC3 + SCAN_TICKS;
		TC5 = TC3 + DEAD_TICKS;
	}
}

isr PT4 input capture {
	if (PT5 high) {
		keypressed = 1;
		key = '7';
	} else if (PT6 high) {
		keypressed = 1;
		key = '8';
	} else if (PT7 high) {
		keypressed = 1;
		key = '9';
	}
}

isr PT3 input capture {
	if (PT5 high) {
		keypressed = 1;
		key = '4';
	} else if (PT6 high) {
		keypressed = 1;
		key = '5';
	} else if (PT7 high) {
		keypressed = 1;
		key = '6';
	}
}

isr PT2 input capture {
	if (PT5 high) {
		keypressed = 1;
		key = '1';
	} else if (PT6 high) {
		keypressed = 1;
		key = '2';
	} else if (PT7 high) {
		keypressed = 1;
		key = '3';
	}
}

void playOrPause() {
	if (playing) {
		playing = false;
		disable PT1 isr
		disable RTI isr
		set PT1 low
	}
	else {
		playing = true;
		enable PT1 isr
		enable RTI isr
		set PT1 high
		TC1 = TCNT + song[note];
	}
}

void clearSong() {
	set all note[] to -1
}

void moveCursor(char key) {
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

void setNote() {
	note[cursorX] = period for cursorY
}

void redraw() {
	clearLCD();

	for (char y = 0; y < ROWS; y++) {
		for (x = 0; x < COLS; x++) {
			if (x == cursorX && y = cursorY) {
				if (note[x] period is for y) {
					writeByteToLCD('d', 1, 50);
				}
				else {
					writeByteToLCD('c', 1, 50);
				}
			}
			else {
				if (note[x] period is for y) {
					writeByteToLCD('l', 1, 50);
				}
				else {
					writeByteToLCD(' ', 1, 50);
				}
			}
		}
	}
}