/*
 * GccApplication1.cpp
 *
 * Created: 2020-04-21 16:58:47
 * Author : Jakub
 */ 

#include <avr/io.h>
#include <avr/sleep.h>
#include <util/delay.h>

/* Pinout:

    PB4->Button
    PB5->LED (invert)

 * PA0: UPDI
 * PB2/PB3: Xtal
 * PC0: Power enable
 * PC1: DIS/CONNECT
 * PC3: DRIVER/AUTON
 * PC2: ENABLE/DISABLE
 * PA5: INPUT D
 * PA4: INPUT A
 * PB4: RUN
 *
 * DevKit:          }-----------{
 *		Vcc )       GND (
 *              PA0 )       PC3 ( Auton
 *              PA1 )       PC2 ( Disable
 *              PA2 )       PC1 ( Connected
 *              PA3 )       PC0 ( Pwr

 *              A-> ) PA4   PB0 (
 *              D-> ) PA5   PB1 (
 *                  ) PA6   PB2 ( Xtal
 *                  ) PA7   PB3 ( Xtal
 *              LED ) PB5   PB4 ( <-S
 *                  \___________/
 */

// DRV ENB CON PWR
#define PIN_CON PIN1_bp
#define PIN_PWR PIN0_bp
#define PIN_ENB PIN2_bp
#define PIN_DRV PIN3_bp
#define IN_DRV PIN5_bp
#define IN_AUT PIN4_bp
#define IN_STR PIN1_bp

// debugging LED on the devkit
#define OUT_LED PIN5_bp

typedef char bool;
#define true 1
#define false 0

bool selDrive (){
    return !(1 & (PORTB_IN >> IN_DRV));
}

bool selAuton (){
    return !(1 & (PORTB_IN >> IN_AUT));
//    return 1;
}

bool start (){
    return !(1 & (PORTB_IN >> IN_STR));
}

int enabled = 0;

uint16_t tbase = 0;
uint16_t mtime = 0;
uint16_t snap = 0;


int select = 0;
int telemetry = 0;


// Testing initially with pullups, will disable later
#if 0
#define IN_PULLUP PORT_PULLUPEN_bm
#else
#define IN_PULLUP 0
#endif


/**
 * Prepare the system to run off the external 32kHz crystal.
 *
 * The ATTINYx16 always boots off internal 16/20MHz RC oscillator,
 * divided by 6, thus 3.33MHz or 2.67MHz
 * We need to run off the external, precise 32.768kHz crystal oscillator,
 * but that has 300ms startup time. To reduce power consumption during
 * this transition, we can first increase the system clock divisor to 64,
 * then switch to the internal 32kHz R/C oscillator with much faster
 * starup time, and only then try to activate the external xtal.
 */
void setupClocks() {
    // divide by 64 ASAP to reduce power to ~1mA
    CPU_CCP = CCP_IOREG_gc;
    CLKCTRL_MCLKCTRLB = (5 << 1) | 1;

    // Switch to the internal RC 32kHz oscillator
    CPU_CCP = CCP_IOREG_gc;
    CLKCTRL_MCLKCTRLA = CLKCTRL_CLKSEL_OSCULP32K_gc;

    // Reduce power further by disabling unused I/Os
    // and properly configuring all other pins
    //PORTB_OUTSET = (1 << OUT_LED); // turn off LED on the dev kit first

    // prepare all outputs to passive state, incuding the power FET
    PORTC_OUTSET = (1 << PIN_PWR) | (1 << PIN_CON) | (1 << PIN_ENB) | (1 << PIN_DRV); 
    PORTC_DIR = 0xFF; // make the whole PORTC output

    PORTB_PIN0CTRL = PORT_ISC_INPUT_DISABLE_gc; 		// Disable input on PB0
    PORTB_PIN1CTRL = IN_PULLUP | PORT_ISC_INTDISABLE_gc;	// PB1, 4, 5 enabled w/o pullups
    PORTB_PIN4CTRL = IN_PULLUP | PORT_ISC_INTDISABLE_gc;    	// Setup input on PB4 with proper pullup mode
    PORTB_PIN5CTRL = IN_PULLUP | PORT_ISC_INTDISABLE_gc;	// Setup input on PB5 with proper pullup mode
//    PORTB_DIRSET = (1 << OUT_LED);		// Enable LED output

    PORTA_PIN0CTRL = PORT_ISC_INPUT_DISABLE_gc; // Disable PA0
    PORTA_PIN1CTRL = PORT_ISC_INPUT_DISABLE_gc; // Disable PA1
    PORTA_PIN2CTRL = PORT_ISC_INPUT_DISABLE_gc; // Disable PA2
    PORTA_PIN3CTRL = PORT_ISC_INPUT_DISABLE_gc; // Disable PA3
    PORTA_PIN4CTRL = PORT_ISC_INPUT_DISABLE_gc; // Disable PA4
    PORTA_PIN5CTRL = PORT_ISC_INPUT_DISABLE_gc; // Disable PA5
    PORTA_PIN6CTRL = PORT_ISC_INPUT_DISABLE_gc; // Disable PA6
    PORTA_PIN7CTRL = PORT_ISC_INPUT_DISABLE_gc; // Disable PA7


    // Wait for the internal RC to boot
    while (0 == (CLKCTRL_MCLKSTATUS & CLKCTRL_OSC32KS_bm));

    // Disable the divider
    CPU_CCP = CCP_IOREG_gc;
    CLKCTRL_MCLKCTRLB = 0;

    // Request external crystal oscillator, enabling it first
    CPU_CCP = CCP_IOREG_gc;
    // 1k startup time, external crystal, no standby, enabled
    CLKCTRL_XOSC32KCTRLA = CLKCTRL_ENABLE_bm; 

    CPU_CCP = CCP_IOREG_gc;
    CLKCTRL_MCLKCTRLA = CLKCTRL_CLKSEL_XOSC32K_gc;

    // Wait for the internal RC to boot
    while (0 == (CLKCTRL_MCLKSTATUS & CLKCTRL_XOSC32KS_bm));

    // Enable the power FET
    PORTC_OUTCLR = (1 << PIN_PWR);

    // done, running on 32kHz external crystal
}

void setupTimer() {
    // Disable other peripherals
//    PRR = _BV(PRTIM1) | _BV(PRUSI) | _BV(PRADC);
    // clear the counter
    TCA0_SINGLE_CNT = 0;
    TCA0_SINGLE_PER = 0xFFFF;	// should be default after boot

    TCA0_SINGLE_CTRLD = 0; // 16-bit single counter mode
    TCA0_SINGLE_CTRLB = TCA_SINGLE_WGMODE_NORMAL_gc; // normal counter

#if F_CPU == 32768
    // For 32768Hz system clock, use division by 256 to have exactly 128Hz timer clock
    TCA0_SINGLE_CTRLA = TCA_SINGLE_CLKSEL_DIV256_gc | 1; // select DIV256 and enable
#else
    // For 125kHz, use division by 1024 to reach roughly 125Hz
    TCA0_SINGLE_CTRLA = TCA_SINGLE_CLKSEL_DIV1024_gc | 1; // select DIV1024 and enable
#endif

    // After this point, the counter is running
}



uint16_t timer() {
    uint16_t val = TCA0_SINGLE_CNT;
    return val;
}

bool comp(){
    uint16_t val = timer() - snap;
    return (val >= mtime) ? true:false; 
}

/*
void blinkByte(int byte) {
    _delay_ms(1000);

    for (int i=0; i<8; i++) {
        int bit = 0x80 & (byte << i);
	PORTB_OUTCLR = PIN5_bm;
        if (bit) _delay_ms(500); else _delay_ms(100);
	PORTB_OUTSET = PIN5_bm;
        if (bit) _delay_ms(500); else _delay_ms(900);
    }
}
*/

bool isRunning() {
    if ((mtime != 0) && comp()) {
        mtime = 0;
    }
    return mtime > 0;
}

/**
 * State machine states:  (// DRV ENB CON PWR)
 *  Init (temporary:	 1111)
 *  Disconnected	(1110)
 *  Disabled Auton	(0000)
 *  Disabled Driver	(1000)
 *  Running Auton	(0100)
 *  Running Driver      (1100)
 *
 * Inputs:
 *  Auton
 *  Driver
 *  Switch
 *  Time > 0
 */

#define ST_INIT 0x0F
#define ST_DC   0x0E
//#define ST_DC   0x2C
#define ST_D_AUT 0x00
#define ST_D_DRV 0x08
#define ST_T_DRV 0x18
#define ST_R_AUT 0x04
#define ST_I_AUT 0x14
#define ST_R_DRV 0x0C

// 0.5s
#define B_LONG_THRESH 64

// 1.5s for delay between press and actual drive time
#define DRIVE_DELAY 192

void stateMachine() {
    uint8_t state = ST_INIT;

    enum {
        // states:
        B_IDLE,
        B_PRESS,
        B_LONG,
        // events:
        B_CLICK,
        B_EXTRA
    } b_state = B_IDLE;

    uint16_t b_base = 0;

    for (;;) {

        // time since last button press change
        uint16_t debounce = timer() - b_base;

        // resolve start press state
        // idle | press | long | release
        if (debounce < 6) { //47 ms
            if (b_state == B_CLICK || b_state == B_EXTRA) {
                b_state = B_IDLE; // reset after delivering the event
            }
        } else if (b_state == B_IDLE && start()) {
            b_base = timer();
            b_state = B_PRESS;
        } else if (b_state == B_PRESS) {
            if (start()) {
                uint16_t b_len = timer() - b_base;
                if (b_len > B_LONG_THRESH) {
                    b_state = B_LONG;
                }
            } else { // released short press
                b_state = B_CLICK;
                b_base = timer();
            }
        } else if (b_state == B_LONG) {
            if (!start()) { // released long press
                b_state = B_EXTRA;
                b_base = timer();
            }
        } else {
            b_state = B_IDLE; // reset after delivering the event
        }


        switch (state) {
            case ST_INIT:
                if (selDrive()) {
                    state = ST_D_DRV;
                } else if (selAuton()) {
                    state = ST_D_AUT;
                } else {
                    state = ST_DC;
                }
                break;

            case ST_DC:
                if (selDrive()) {
                    state = ST_D_DRV;
                } else if (selAuton()) {
                    state = ST_D_AUT;
                } else if (b_state == B_EXTRA) {
                    // start in middle position, unlimited auton
                    state = ST_I_AUT;
                }
                break;

            case ST_D_DRV:
                // handle mode change first
                if (!selDrive()) {
                    if (selAuton()) {
                        state = ST_D_AUT;
                    } else {
                        state = ST_DC;
                    }
                    break;
                }

                if (b_state == B_CLICK) {
                    snap = timer();
                    mtime = DRIVE_DELAY + 105*128;   // 1:45 match driver run
                    state = ST_T_DRV;
                } else if (b_state == B_EXTRA) {
                    snap = timer();
                    mtime = DRIVE_DELAY + 60*128;   // 1:00 skills driver run
                    state = ST_T_DRV;
                }
                break;

            case ST_T_DRV:                // Initial driver delay
                // handle mode change first
                if (!selDrive()) {
                    if (selAuton()) {
                        state = ST_D_AUT;
                    } else {
                        state = ST_DC;
                    }
                    break;
                }

                if (b_state == B_CLICK) { // cancel while waiting
                    state = ST_D_DRV;
                }

                if ((timer() - snap) > DRIVE_DELAY) {
                    state = ST_R_DRV;
                }
                break;

            case ST_D_AUT:
                // handle mode change first
                if (!selAuton()) {
                    if (selDrive()) {
                        state = ST_D_DRV;
                    } else {
                        state = ST_DC;
                    }
                    break;
                }

                if (b_state == B_CLICK) {
                    snap = timer();
                    mtime = 15*128;   // 0:15 match auton run
                    state = ST_R_AUT;
                } else if (b_state == B_EXTRA) {
                    snap = timer();
                    mtime = 60*128;   // 1:00 skills auton run
                    state = ST_R_AUT;
                }
                break;

            case ST_R_AUT:
                // handle mode change first
                if (!selAuton()) {
                    if (selDrive()) {
                        state = ST_D_DRV;
                    } else {
                        state = ST_DC;
                    }
                    break;
                }

                if (!isRunning() || (b_state == B_CLICK)) { // timeout or cancel
                    state = ST_D_AUT;
                }
                break;

            case ST_I_AUT: // infinite auton
                // handle mode change first
                if (selDrive()) {
                    state = ST_D_DRV;
                } else if (selAuton()) {
                    state = ST_D_AUT;
                } else if (b_state == B_CLICK) { // cancel
                    state = ST_DC;
                }

                break;

            case ST_R_DRV:
                // handle mode change first
                if (!selDrive()) {
                    if (selAuton()) {
                        state = ST_D_AUT;
                    } else {
                        state = ST_DC;
                    }
                    break;
                }

                if (!isRunning() || (b_state == B_CLICK)) { // timeout or cancel
                    state = ST_D_DRV;
                }
                break;
        }

        PORTC_OUT = state;
    }
}


int main(void){
    setupClocks();
    setupTimer();

//    blinkByte(CLKCTRL_MCLKSTATUS);


    stateMachine();
}

