//#
//# blind_safe_attiny1634.cpp
//#
//# Created: 12/3/2014 8:40:30 PM
//# Author: Ross
//#
//#
//# Filename:		blind_safe_attiny1634.cpp
//#
//# Description:	This program is the main program for the blind spot detector
//#
//#								I/O's
//#				        	  __________
//#				INT_ACC1-----|			|-----SDA
//#				INT_ACC2-----|	 MCU	|-----SCL
//#				 TRIGGER-----|			|-----CE_REG
//#					ECHO-----|			|-----LED_CTRL
//#					CE_REG1--|			|-----MISO
//#				BATT_READ----|			|-----MOSI
//#					RESET----|			|-----SCK
//#				VCC_SOLAR----|			|-----BR_CTRL
//#					         |__________|
//#
//#
//# Authors:     	Ross Yeager
//#
//#END_MODULE_HEADER//////////////////////////////////////////////////////////
#include <avr/sleep.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <avr/io.h>
#include "Arduino.h"
#include "MMA8452REGS.h"
#include "Ultrasonic.h"
#include "MMA8452.h"

#include "i2c.h"
#define RDV_DEBUG
//#define RDV_DEBUG_ACC_DATA

/************************************************************************/
//PIN/PORT DEFINITIONS:
/************************************************************************/
#define TRIGGER			PORTA0	//OUTPUT
#define CE_REG_LED		PORTA3	//OUTPUT SENSOR AND LED OUTPUT
#define BATT_READ		PORTA6	//INPUT ADC
#define BATT_RD_CNTL	PORTA7	//OUTPUT
#define INT2_ACC		PORTA4	//INPUT INTERRUPT DATA READY (PCIE1)
#define LED_CNTL1		PORTA5	//OUTPUT PWM AMBER LED
#define VCC_SOLAR		PORTB3	//INPUT ADC
#define ECHO_3V3		PORTB0	//INPUT

//#define TWO_LEDS			//TODO: define this when new LED is in place
//#define BRIGHTNESS_CTNL	//TODO: define this if you want to control brightness based on daylight
#define LED_CNTL2		0	//OUTPUT PWM RED LED		//TODO: add this pin somewhere
#define CE_REG1_SOLAR	PORTC2	//OUTPUT OUTPUT SOLAR PANEL OUTPUT
#define INT1_ACC		PORTC0	//INPUT INTERRUPT MOTION DETECTION (PCIE0)


#define CE_REG1_PORT		PORTC
#define LED1_PORT			PORTA
#define LED2_PORT			PORTA
#define CE_REG_PORT			PORTA
#define BATT_RD_CNTL_PORT	PORTA

#define LED1_DDR		DDRA
#define CE_REG_DDR		DDRA
#define CE_REG1_DDR		DDRC

#define MAX_SONAR_DISTANCE 500
#define MIN_SONAR_DISTANCE 100
#define LED_ON_TIME		   500

#define INT1_PCINT		PCINT12
#define INT2_PCINT		PCINT4
#define INT1_PCIE		PCIE2
#define INT2_PCIE		PCIE0
#define INT1_PCMSK		PCMSK2
#define INT2_PCMSK		PCMSK0
#define INT1_PIN		PINC
#define INT2_PIN		PINA
#define ALL_INTS		10

//BIT-BANG I2C PINS (define in i2c.h)
#define SOFT_SCL		PORTA2	//INOUT
#define SOFT_SDA		PORTA1	//INOUT

/************************************************************************/
//ACCELEROMETER CONSTANTS: these all need to be translated to attributes
/************************************************************************/
#define ACCEL_ADDR				0x1C	// I2C address for first accelerometer
#define SCALE					0x08	// Sets full-scale range to +/-2, 4, or 8g. Used to calc real g values.
#define DATARATE				0x05	// 0=800Hz, 1=400, 2=200, 3=100, 4=50, 5=12.5, 6=6.25, 7=1.56
#define SLEEPRATE				0x03	// 0=50Hz, 1=12.5, 2=6.25, 3=1.56
#define ASLP_TIMEOUT 			600		// Sleep timeout value until SLEEP ODR if no activity detected 640ms/LSB
#define MOTION_THRESHOLD		16		// 0.063g/LSB for MT interrupt (16 is minimum to overcome gravity effects)
#define MOTION_DEBOUNCE_COUNT 	1		// IN LP MODE, TIME STEP IS 160ms/COUNT
#define I2C_RATE				100
#define NUM_AXIS 3
#define NUM_ACC_DATA (DATARATE * NUM_AXIS)

typedef enum{
	F80000 = 0,
	F40000,
	F20000,
	F10000,
	F5000,
	F1250,
	F625,
	F156
} AccOdr;

/************************************************************************/
//BATTERY/SOLAR CONSTANTS:
/************************************************************************/
#define LOW_BATTERY				3.5f
#define CHARGED_BATTERY			3.8f
#define DAYTIME					2.0f
#define BATTERY_CHECK_INTERVAL  300

/************************************************************************/
//DATA STRUCTURES:
/************************************************************************/
typedef struct battery_t{
	bool daylight;
	long solar_vcc;
	long battery_vcc;
	long mcu_vcc;
	uint8_t brightness;
}Battery;

/************************************************************************/
//PROTOTYPES
/************************************************************************/
void toggle_led(uint8_t blinks);
void shut_down_sensor(bool ce_led);
void init_accelerometer(void);
void initialize_pins(void);
bool check_moving(void);
long read_mcu_batt(void);
bool clear_acc_ints(void);
void disable_int(uint8_t pcie);
void enable_int(uint8_t pcie);
void deep_sleep_handler(bool still);
void ISR_notify_timer(void);
void setup(void);

/************************************************************************/
//GLOBAL VARIABLES
/************************************************************************/
static volatile bool got_slp_wake;
static volatile bool got_data_acc;
static volatile bool _sleep;
static volatile bool driving;
static volatile bool time_up;
static volatile uint16_t batt_counter;
static volatile uint8_t intSource;

static bool accel_on;
static unsigned long usec;
static float range;

static int16_t accelCount[3];  				// Stores the 12-bit signed value
static float accelG[3];  						// Stores the real accel value in g's
AccOdr f_odr = (AccOdr)DATARATE;
MMA8452 accel = MMA8452(ACCEL_ADDR);
Ultrasonic ultrasonic;
Battery battery;


//#START_FUNCTION_HEADER//////////////////////////////////////////////////////
//#
//# Description: 	Main loop
//#
//# Parameters:		None
//#
//# Returns: 		Nothing
//#
//#//END_FUNCTION_HEADER////////////////////////////////////////////////////////
int main(void)
{
	setup();
	
    while(1)
    {
		//ACCELEROMETER DATA IS READY
		if (got_data_acc && accel_on)
		{
			got_data_acc = false;
			accel.readAccelData(accelCount);  // Read the x/y/z adc values, clears int
			
			//sequentially check for motion detection
			if((INT1_PIN & (1 << INT1_ACC)))
			{
				driving = check_moving();	//we will go through one more time before, maybe break out here?
				if(!driving)
				{
					shut_down_sensor(false);
				}
			}
			else
			driving = true;
			
			_sleep = true;
			
			if(++batt_counter > BATTERY_CHECK_INTERVAL)
			{
				batt_counter = 0;
				//handle_battery_mgmt();	//TODO: battery management
			}
			
			CE_REG_PORT |= (1 << CE_REG_LED);	//enable power to sonar and LED
			_delay_us(50);	//TODO: may need to adjust delay here to optimize bringup time
			
			range = 0;
			disable_int(ALL_INTS);
			usec = ultrasonic.timing(50000);
			range = ultrasonic.convert(usec, ultrasonic.CM);
			
			if(range < MAX_SONAR_DISTANCE && range >= MIN_SONAR_DISTANCE)
			{
//#ifdef TWO_LEDS
				//if(Battery.battery_vcc < LOW_BATTERY)
					//LED2_PORT |= (1 << LED_CNTL2);
				//else
					//LED1_PORT |= (1 << LED_CNTL1);
//#else
				//LED1_PORT |= (1 << LED_CNTL1);
//#endif
				////TODO: brightness control
				////#ifdef BRIGHTNESS_CTNL
				////#ifdef TWO_LEDS
				////if(Battery.battery_vcc < LOW_BATTERY)
				////analogWrite(LED2_PIN, Battery.brightness);
				////else
				////analogWrite(LED1_PIN, Battery.brightness);
				////#else
				////analogWrite(LED1_PIN, Battery.brightness);
				////#endif
				////#endif

				LED1_PORT |= (1 << LED_CNTL1);
				_delay_ms(LED_ON_TIME);	//TODO: some sort of sleep mode during this time?
				//turn off the power to sonar and led
				CE_REG_PORT &= ~(1 << CE_REG_LED);
				enable_int(ALL_INTS);
			}
			else
				enable_int(ALL_INTS);
			
			LED1_PORT &= ~(1 << LED_CNTL1);
			
			

//#ifdef TWO_LEDS
			//if(Battery.battery_vcc < LOW_BATTERY)
				//LED2_PORT &= ~(1 << LED_CNTL2);
			//else
				//LED1_PORT &= ~(1 << LED_CNTL1);
//#else
			//LED1_PORT &= ~(1 << LED_CNTL1);
//#endif
			
		}
		
		//ACCELEROMETER CHANGED INTO SLEEP/AWAKE STATE
		if(got_slp_wake)
		{
			got_slp_wake = false;
			driving = check_moving();
			if(driving)
			{
				//handle_battery_mgmt();	//TODO: battery management
			}
			_sleep = true;		//go into driving state mode
		}
		
		if(_sleep)
		{
			deep_sleep_handler(driving);
		}
    }
}


//#START_FUNCTION_HEADER//////////////////////////////////////////////////////
//#
//# Description: 	Initialization function.  Configures IO pins, enables interrupts.
//#					Heartbeat checks the accelerometerS.
//#					in debug, it can print out initialization stats.
//#
//# Parameters:		None
//#
//# Returns: 		Nothing
//#
//#//END_FUNCTION_HEADER////////////////////////////////////////////////////////
void setup()
{
	
	//initialize the pins
	initialize_pins();
	_delay_ms(1500);
	
	sei();								//ENABLE EXTERNAL INTERRUPT FOR WAKEUP
	got_slp_wake = false;
	got_data_acc = false;
	_sleep = false;
	driving = false;
	
	//toggle_led(2);
	_delay_ms(500);
	shut_down_sensor(false);
	
	//initialize battery monitoring system
	battery.battery_vcc = 0;
	battery.solar_vcc = 0;
	battery.daylight = false;
	battery.mcu_vcc = 0;
	battery.brightness = 255;	//default to full brightness
	batt_counter = 0;
	
	ADCSRA = 0;	//disable ADC
	
	if (accel.readRegister(WHO_AM_I) == 0x2A) 						// WHO_AM_I should always be 0x2A
	{
		accel_on = true;
	}
	else
	{
		accel_on = false;
	}
	
	init_accelerometer();
	_delay_ms(1500);
	toggle_led(2);
}


//#START_FUNCTION_HEADER//////////////////////////////////////////////////////
//#
//# Description: 	Toggles the LED specified number of times
//#
//# Parameters:		Number of blinks
//#
//# Returns: 		Nothing
//#
//#//END_FUNCTION_HEADER////////////////////////////////////////////////////////
void toggle_led(uint8_t blinks)
{
	LED1_DDR |= (1 << LED_CNTL1);
	CE_REG_DDR |= (1 << CE_REG_LED);
	LED1_PORT |= (1 << LED_CNTL1);
	CE_REG_PORT |= (1 << CE_REG_LED);
	for(int i = 0; i< blinks; i++)
	{
		LED1_PORT |= (1 << LED_CNTL1);
		_delay_ms(500);
		LED1_PORT &= ~(1 << LED_CNTL1);
		_delay_ms(500);
	}
	
}

//#START_FUNCTION_HEADER//////////////////////////////////////////////////////
//#
//# Description: 	This checks to see if we are moving or not
//#
//# Parameters:		None
//#
//# Returns: 		Nothing
//#
//#//END_FUNCTION_HEADER////////////////////////////////////////////////////////
bool check_moving()
{
	bool still = false;
	intSource= accel.readRegister(INT_SOURCE) & 0xFE; //we don't care about the data interrupt here
	accel.readRegister(FF_MT_SRC);	//CLEAR MOTION INTERRUPT

	switch(intSource)
	{
		case 0x84:		//MOTION AND SLEEP/WAKE INTERRUPT (if even possible?)
		case 0x80:		//SLEEP/WAKE INTERRUPT
		{
			uint8_t sysmod = accel.readRegister(SYSMOD);
			//accel.readRegister(FF_MT_SRC);	//CLEAR MOTION INTERRUPT

			if(sysmod == 0x02)    		//SLEEP MODE
			still = true;
			else if(sysmod == 0x01)  	//WAKE MODE
			still = false;
			break;
		}
		case 0x04:						//MOTION INTERRUPT
		default:
		break;
	}
	clear_acc_ints();			//clear interrupts at the end of this handler

	return (still ? false : true);
}

//#START_FUNCTION_HEADER//////////////////////////////////////////////////////
//#
//# Description: 	This powers down the ultrasonic and led power and turns off
//#					the battery reading voltage divider
//#
//# Parameters:		None
//#
//# Returns: 		Nothing
//#
//#//END_FUNCTION_HEADER////////////////////////////////////////////////////////
void shut_down_sensor(bool ce_led)
{
	//turn off the LED and sensor/led power
	LED1_PORT &= ~(1 << LED_CNTL1);
	if(ce_led)
		CE_REG_PORT &= ~(1 << CE_REG_LED);
	else
		CE_REG_PORT |= (1 << CE_REG_LED);
	//turn on charging //TODO: 
	//CE_REG1_PORT |= (1 << CE_REG1_SOLAR);
	CE_REG1_PORT &= ~(1 << CE_REG1_SOLAR);
	
	//disable battery reads
	BATT_RD_CNTL_PORT &= ~(1 << BATT_RD_CNTL);
	ADCSRA = 0;	//disable ADC
}

//#START_FUNCTION_HEADER//////////////////////////////////////////////////////
//#
//# Description: 	Initializes the accelerometer and enables the interrupts
//#
//# Parameters:		None
//#
//# Returns: 		Nothing
//#
//#//END_FUNCTION_HEADER////////////////////////////////////////////////////////
void init_accelerometer(){
	
	enable_int(ALL_INTS);
	
	accel.initMMA8452(SCALE, DATARATE, SLEEPRATE, ASLP_TIMEOUT, MOTION_THRESHOLD, MOTION_DEBOUNCE_COUNT);
}

//#START_FUNCTION_HEADER//////////////////////////////////////////////////////
//#
//# Description: 	Sets up the pin configurations
//#
//# Parameters:		None
//#
//# Returns: 		Nothing
//#
//#//END_FUNCTION_HEADER////////////////////////////////////////////////////////
void initialize_pins()
{
	//outputs
	DDRA =  (1 << CE_REG_LED) | (1 << TRIGGER) | (1 << BATT_RD_CNTL) | (1 << LED_CNTL1);
	DDRC =  (1 << CE_REG1_SOLAR);
	PORTA = 0;	
	PORTB = 0;
	PORTC &= ~(1 << CE_REG1_SOLAR);
	
	//inputs
	DDRA &=  ~((1 << BATT_READ) | (1 << INT2_ACC) | (1 << SOFT_SDA) | (1 << SOFT_SCL));
	PORTA |= (1 << INT2_ACC);	//activate pullups
	
	DDRB &=  ~((1 << VCC_SOLAR) | (1 << ECHO_3V3));
	DDRB |= (1 << ECHO_3V3);
	
	DDRC &=  ~(1 << INT1_ACC);
	PORTC |= (1 << INT1_ACC);	//activate pullups except on I2C pins
	
	//TODO: PWM SETUP
}

//#START_FUNCTION_HEADER//////////////////////////////////////////////////////
//#
//# Description: 	Puts the unit into sleep while enabling proper interrupts to
//#					exit sleep mode.  In normal mode, we want to sleep in between
//#					data ready acquisitions to maximize power.  When no motion is present,
//#					we only want to be woken up by BLE or movement again, not data
//#					ready.
//#
//# Parameters:		driving --> 	false = disable acc data interrupts
//#									true = enable acc data interrupts
//#
//# Returns: 		Nothing
//#
//#//END_FUNCTION_HEADER////////////////////////////////////////////////////////
void deep_sleep_handler(bool driving)
{
	got_slp_wake = false;
	got_data_acc = false;
	set_sleep_mode(SLEEP_MODE_PWR_DOWN);
	sleep_enable();
	cli();
	//sleep_bod_disable();
	if(driving)
	{
		enable_int(INT2_PCIE);
		disable_int(INT1_PCIE);
	}
	else
	{
		enable_int(INT1_PCIE);
		disable_int(INT2_PCIE);
	}
	if(!clear_acc_ints())
	return;
	sei();
	sleep_cpu();
	sleep_disable();
	_sleep = false;
}

//#START_FUNCTION_HEADER//////////////////////////////////////////////////////
//#
//# Description: 	Reads the specified registers to clear all interrupts for the
//#					accelerometer.
//#					INT_SOURCE | FF_MT_SRC | ACCELEROMETER DATA
//#
//# Parameters:		None
//#
//# Returns: 		Nothing
//#
//#//END_FUNCTION_HEADER////////////////////////////////////////////////////////
bool clear_acc_ints()
{
	if(accel.readRegister(INT_SOURCE) == ~0u)
	return false;
	if(accel.readRegister(FF_MT_SRC) == ~0u)
	return false;
	accel.readAccelData(accelCount);
	return true;
}

//#START_FUNCTION_HEADER//////////////////////////////////////////////////////
//#
//# Description: 	Enables the corresponding interrupt bank.
//#
//# Parameters:		Interrupt bank to enable.  Default is all interrupts enabled.
//#
//# Returns: 		Nothing
//#
//#//END_FUNCTION_HEADER////////////////////////////////////////////////////////
void enable_int(uint8_t pcie)
{
	switch(pcie)
	{
		case INT1_PCIE:
		{
			INT1_PCMSK |= (1 << INT1_PCINT);	//INT1_ACC Interrupt
			break;
		}
		case INT2_PCIE:
		{
			INT2_PCMSK |= (1 << INT2_PCINT);	//INT2_ACC Interrupt
			break;
		}
		default:
		{
			INT1_PCMSK |= (1 << INT1_PCINT);
			INT2_PCMSK |= (1 << INT2_PCINT);
			break;
		}
	}
	
	if((pcie == PCIE0) || (pcie == PCIE2))
		GIMSK |= (1 << pcie);
	else
		GIMSK |= ((1 << INT1_PCIE) | (1 << INT2_PCIE));
}


//#START_FUNCTION_HEADER//////////////////////////////////////////////////////
//#
//# Description: 	Disables the corresponding interrupt bank.
//#
//# Parameters:		Interrupt bank to disable.  Default is all interrupts disabled.
//#
//# Returns: 		Nothing
//#
//#//END_FUNCTION_HEADER////////////////////////////////////////////////////////
void disable_int(uint8_t pcie)
{
	switch(pcie)
	{
		case PCIE2:
		{
			INT1_PCMSK &= ~(1 << INT1_PCINT);	//INT1_ACC Interrupt
			break;
		}
		case PCIE0:
		{
			INT2_PCMSK &= ~(1 << INT2_PCINT);	//INT2_ACC Interrupt
			break;
		}
		default:
		{
			INT1_PCMSK &= ~(1 << INT1_PCINT);
			INT2_PCMSK &= ~(1 << INT2_PCINT);
			break;
		}
	}
	
	if((pcie == PCIE0) || (pcie == PCIE2))
		GIMSK &= ~(1 << pcie);
	else
		GIMSK = 0;
	
}

//#START_FUNCTION_HEADER//////////////////////////////////////////////////////
//#
//# Description: 	ISR for the PCINT2 bus interrupt.  This is an external interrupt from
//#					the accelerometer that is triggered by filtered motion or if the
//#					accelerometer is entering or exiting sleep mode.
//#
//# Parameters:		Interrupt vector
//#
//# Returns: 		Nothing
//#
//#//END_FUNCTION_HEADER////////////////////////////////////////////////////////
ISR(PCINT2_vect)
{
	cli();
	if((INT1_PIN & (1 << INT1_ACC)))
	{
		got_slp_wake = true;
		
	}
	sei();
}

//#START_FUNCTION_HEADER//////////////////////////////////////////////////////
//#
//# Description: 	ISR for the PCINT0 bus interrupt.  This is an external interrupt from
//#					the accelerometer that is triggered by the accelerometer data being ready.
//#
//# Parameters:		Interrupt vector
//#
//# Returns: 		Nothing
//#
//#//END_FUNCTION_HEADER////////////////////////////////////////////////////////
ISR(PCINT0_vect)
{
	cli();
	if((INT2_PIN & (1 << INT2_ACC)))
	{
		got_data_acc = true;
		disable_int(INT1_PCIE);	//disable motion interrupt and sequential check instead
	}
	sei();
}