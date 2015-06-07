/* lokbasis3b - f�r ATMega2561 im (Wiznet /) Raspi-Betrieb
 *
 * main.c
 *
 *	Version: 0.2
 *  Created on: 14.12.2014 - 28.05.2015
 *  Author: Michael Brunnbauer
 */


#ifndef F_CPU
#define F_CPU 16000000UL	// f�r 16MHz CPU-Takt
#endif


#ifndef EEMEM	//#include <avr/eeprom.h> ist zwar angegeben, EEMEM Definition funktioniert aber trotzdem nicht!!
#define EEMEM __attribute__((section(".eeprom")))	// deshalb hier nochmal
#endif

#include <avr/io.h>
#include <string.h>		// f�r "strcmp"
#include <stdlib.h>		// f�r "itoa"
#include <util/delay.h>	// f�r delay_ms()
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <avr/eeprom.h>
#include <util/twi.h>	// f�r I2C

#include "lokbasis_hwdef.h"		// Hardware-Definitionen f�r die verschiedenen Boards
#include "main.h"
#include "uart.h"
#include "commands.h"			// Befehle auswerten
#include "wlan.h"
#include "funktionen.h"			// allgemeine Funktionen (Hardware und andere)
#include "speed.h"				// Geschwindigkeit anpassen
#include "ledc.h"				// LED-Controller
#include "i2cmaster.h"			// I2C Funktionen
#include <avr/wdt.h>			// watchdog


// Watchdog Deaktivierung im Startcode. Notwendig, da Watchdog f�r Software-Reset (->Bootloader) verwendet wird
// Function Prototype
void wdt_init(void) __attribute__((naked)) __attribute__((section(".init3")));


// Function Implementation
void wdt_init(void)
{
    MCUSR = 0;
    wdt_disable();
    return;
}



//----------------------------------------------------------------------------------------------
volatile unsigned char state = 0;		// Status-Variable f�r Interrupts und Main-Schleife

unsigned int timer5_count = 0;			// z�hler f�r timer5 (f�r isr, generelle Zeitintervalle)

char wlan_string[UART_MAXSTRLEN+1]="";  // globaler String zum Abspeichern des vom WLAN empfangen Strings in read_wlan


unsigned int speed = 0;					// globale variable f�r den Motor-Speed (reale Geschwindigkeit)
unsigned int speed_soll = 0;			// globale variable f�r den Motor-Speed - Vorgabe vom Controller
unsigned char richtung = RICHTUNG_VW;			// globlae variable f�r die Richtung (vorw�rts = 1, r�ckw�rts = 0)
unsigned char richtung_soll = RICHTUNG_VW;		// globlae variable f�r die Richtung - Vorgabe vom Controller (vorw�rts = 1, r�ckw�rts = 0)
unsigned char speedstep_korrektur = 0;			// Korrekturvariable, falls 8bit oder 9bit pwm statt 10bit verwendet wird
												// gibt an, um wieviele bit der speedwert geshiftet werden muss, damit er f�r die 10bit Verarbeitung passt (im timer3 interrupt)

unsigned char alivecount = 0;	// globlae Variable zum Z�hlen der Meldungen von der Gegenstelle
volatile char alivesecs = 0;	// globlae Variable die z�hlt, wie lange die Z�hlung geht -> man kann variable bestimmen, in welchem intervall gepr�ft wird

volatile unsigned char motor_reg = 1;	// Variable f�r Motor-Regelung ein/aus (auch f�r isr verwendet)
uint8_t motorerror = 0;					// Errorcode von Motorcontroller: 0 = kein Error

//ADC
volatile uint8_t adcchannel = 0;		// aktueller ADC channel 0-7
volatile uint8_t adcreadcount = 0;		// counter f�r Lesevorg�nge pro ADC channel
volatile unsigned int adcvalue[8][4] = { {0,0,0,0},	// 8 cannels, 4 Werte werden gelesen
                     	 	 	  	     {0,0,0,0},
                     	 	 	  	     {0,0,0,0},
                     	 	 	  	     {0,0,0,0}};



// LokData im Flash
const char dev_swname[] PROGMEM = "lokbasis";     	// -> keine �nderung durch User -> flash
const char dev_swversion[] PROGMEM = "0.04";   		// -> keine �nderung durch User -> flash

//Strings im Flash f�r CMD-R�ckmeldungen �ber WLAN
const char txtp_cmdend[] PROGMEM = ">";						// Befehlsende-Zeichen
const char txtp_errmotor[] PROGMEM = "<error:motor:";		// Motor-Error
const char txtp_sd[] PROGMEM = "<sd:";						// Speed-R�ckmeldung
const char txtp_pong[] PROGMEM = "<pong>";					// Antwort f�r den ping Befe
const char txtp_default_lok_name[] PROGMEM = "Lok X";			// Standardwert f�r EEData Lokname
const char txtp_default_owner_name[] PROGMEM = "TheOwner";		// Standardwert f�r EEData Owner-Name

// Befehle
const char txtp_cmd_stop[] PROGMEM = "stop";
const char txtp_cmd_off[] PROGMEM = "off";
const char txtp_cmd_stopall[] PROGMEM = "stopall";
const char txtp_cmd_richtung[] PROGMEM = "richtung:";
const char txtp_cmddata_vw[] PROGMEM = "vw";
const char txtp_cmddata_rw[] PROGMEM = "rw";
const char txtp_cmd_sd[] PROGMEM = "sd:";
const char txtp_cmd_ping[] PROGMEM = "ping";
const char txtp_cmd_l1[] PROGMEM = "l1:";
const char txtp_cmd_l0[] PROGMEM = "l0:";
const char txtp_cmd_reset[] PROGMEM = "reset";
const char txtp_cmd_onameset[] PROGMEM = "onameset:";
const char txtp_cmddata_start[] PROGMEM = "start:";
const char txtp_cmddata_add[] PROGMEM = "add:";
const char txtp_cmddata_end[] PROGMEM = "end:";


const char txtp_cmd_[] PROGMEM = "";


int main(void)
{
	char test[UART_MAXSTRLEN+1] = "";			// f�r string-Bearbeitungen (R�ckmeldungen usw.)
	uint32_t loop_count = 0;					// Z�hler f�r Hauptschleife
	uint32_t last_loop_count = 0;					// letztes g�ltiges Ergebnis des Z�hlers f�r Hauptschleife



	// ========================  Hardware Initialisierung  ========================================================

	init_uart(UART_NR_WLAN, UART_SETTING_WLAN);	// WLAN Daten Empfang aktivieren

	init_timer5(); 	// timer5 mit 244Hz (4ms)

	init_motorctrl();	// Ausg�nge f�r Motorsteuerung, PWM

	#if defined( LEDC_PCA9622 ) || defined( LEDC_TLC59116 )
	i2c_init();
	ledcontrol_init(LEDC1);	// LED-Controller TODO: Funktion testen
	#endif	
	
	setbit(DDRD,PD6);	// Pin als Ausgang definieren - f�r Testsignal
	setbit(DDRD,PD7);	// Pin als Ausgang definieren - f�r Testsignal
	
	//init_gpios();		// frei verf�gbare GPIOs als Ausgang definieren

	sei();	// Interrupts aufdrehen

	// ========================  Hardware Initialisierung abgeschlossen  ================================================


	uart0_puts("<MC start>");

	setbit(PORTD,PD6);	// TEST: auf HI

//-------------------------------------------------------------------------------------------------------------
//------                       H A U P T S C H L E I F E                                               --------
//-------------------------------------------------------------------------------------------------------------








	while (1)	// Hauptschleife (endlos)
	{
		loop_count++;	// Z�hler f�r Hauptschleife

		PORT_TESTSIGNAL ^= (1<<TESTSIGNAL);	// Signale bei jedem Durchlauf togglen (oder 1x pro Sekunde weiter unten)


		if (alivesecs >= 3)	// //TODO lokdata // Pr�fintervall = ALIVE_INTERVAL Sekunden! -> Lok stoppt nach ALIVE_INTERVAL sek herrenloser Fahrt
		{
			if (alivecount == 0)	// pr�fen, ob in diesem Pr�fintervall Meldungen der Gegenstelle einglangt sind
			{
				//alone = 1;

			}
				alivecount = 0;	// Z�hler r�cksetzen
				alivesecs = 0;
		}

		//Error von MotorController checken (passt f�r phb01 und Pololu 24v20) -> Error wird in motorerror gespeichert
		checkMotorStatus();  // TODO: Funktion testen


		if (state & STATE_5X_PRO_SEK)
		{
			set_speed();		// setzt die aktuelle Geschwindigkeit und checkt, ob speed-�nderungen "gesoftet" werden m�ssen

			cli();
			state &= ~STATE_5X_PRO_SEK;	// state-flag zur�cksetzen
			sei();
		}

		if (state & STATE_1X_PRO_SEK)		// Meldung an Server bei == 1 / Steuerger�t (ca. 1x pro Sekunde)
		{
			memset(test, 0, UART_MAXSTRLEN+1);	// string leeren
			strlcpy_P(test, txtp_sd, 5);	// R�ckmeldung der Geschwindigkeit	//bei der L�nge immer 0-Zeichen mitz�hlen!
			itoa(speed, test+4, 10);
			strlcat_P(test, txtp_cmdend, UART_MAXSTRLEN+1);	// will l�nge des kompletten "test" buffers+0
			uart0_puts(test);



			/*
			// Test-Ausgabe MotorStatus
			memset(test, 0, UART_MAXSTRLEN+1);	// string leeren
			strlcpy_P(test, txtp_errmotor, 14);
			ltoa(motorerror, test+13, 10);
			strlcat_P(test, txtp_cmdend, UART_MAXSTRLEN+1); // will L�nge des kompletten "test" buffers+0
			uart0_puts(test);
			*/


			/*
			ltoa(loop_count, test, 10);	// testweise R�ckmeldung des loopcount -> sp�ter an handy
			log_puts_P("loopcount = ");
			log_puts(test);
			log_puts_P("\r\n");
			*/

			// TODO: Spannungsmeldung Schiene, Akku und ev. Motoren

			last_loop_count = loop_count;	// ermittelten loopcount sichern
			loop_count = 0;	// loopcount sek�ndlich zur�cksetzen -> ergibt loops/s
			cli();
			state &= ~STATE_1X_PRO_SEK;	// state-flag zur�cksetzen
			sei();

		}


		if (uart0_available() > 0) { check_wlan_cmd(); }	// checken, ob WLAN-Daten vorhanden sind (wenn ja -> verarbeiten)


	}	// Ende Hauptschleife (endlos)

} // main ende




//-----------------------------------------------------------------------------------------
//----------------   Interrupts   ---------------------------------------------------------
//-----------------------------------------------------------------------------------------




//-------------------------------------------------------------------------------------------------------------
// isr f�r timer 5 output compare match A interrupt: 244 Hz = 4ms
ISR(TIMER5_COMPA_vect) {

	timer5_count++;


	if (timer5_count % 48 == 0)	// 5x pro Sekunde. state: STATE_5X_PRO_SEK
	{
		state |= STATE_5X_PRO_SEK;	// state setzen
	}

	if (timer5_count == 244)	// 1x pro Sekunde
	{
		state |= STATE_1X_PRO_SEK;	// state setzen
		alivesecs++;
		timer5_count = 0;	// es wird nur bis 30 gez�hlt -> 1 Sekunde, dann wieder bei 0 anfangen
	}

}


// ADC Interrupt routine

ISR(ADC_vect) {

	unsigned int val;

	//TODO: ACHTUNG: alles noch alt: muss noch ge�ndert werden (ist aus "lokbasis1 - Kopie v0.7")
	// var:
	// volatile uint8_t adcchannel = 0;			// aktueller ADC channel (0-7)
	// volatile uint8_t adcreadcount = 0;		// counter f�r Lesevorg�nge pro ADC channel
	// volatile unsigned int adcvalue[adcchannel][adcreadcount-1] // 8 cannels, 4 Werte werden gelesen (der erste wird ignoriert!)

	// auslesen und in richtiger Variable speichern
	// 3 oder 5 werte lesen, den 1. verwerfen
	// danach auf n�chsten port umstellen
	// dann in der Hauptschleife den Wert mitteln

	// alt: nach der Methode kracht es!!! Reboots osw!! solbad der Interrupt deaktiviert ist, passt alles!!


	val = ADC;	//Wert muss immer gelesen werden

	if (adcreadcount > 0)
	{
		adcvalue[adcchannel][adcreadcount-1] = val;
	}

	adcreadcount++;

	if (adcreadcount > 4)
	{
		adcreadcount = 0;
		adcchannel++;
		ADMUX = (ADMUX & ~(0x1F)) | (adcchannel & 0x1F);	// n�chsten channel einstellen
	}

	if (adcchannel > 7) { adcchannel = 0; }

	setbit(ADCSRA,ADSC);	// start next conversion


}



