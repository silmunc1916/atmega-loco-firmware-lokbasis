/* lokbasis3b - für ATMega2561 im (Wiznet /) Raspi-Betrieb
 *
 * main.c
 *
 *	Version: 0.04
 *  Created on: 14.12.2014 - 20.06.2015
 *  Author: Michael Brunnbauer
 */


#ifndef F_CPU
#define F_CPU 16000000UL	// für 16MHz CPU-Takt
#endif


#ifndef EEMEM	//#include <avr/eeprom.h> ist zwar angegeben, EEMEM Definition funktioniert aber trotzdem nicht!!
#define EEMEM __attribute__((section(".eeprom")))	// deshalb hier nochmal
#endif

#include <avr/io.h>
#include <string.h>		// für "strcmp"
#include <stdlib.h>		// für "itoa"
#include <util/delay.h>	// für delay_ms()
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <avr/eeprom.h>
#include <util/twi.h>	// für I2C
#include <avr/wdt.h>			// watchdog

#include "lokbasis_hwdef.h"		// Hardware-Definitionen für die verschiedenen Boards
#include "main.h"
#include "uart.h"
#include "commands.h"			// Befehle auswerten
#include "eedata.h"
#include "wlan.h"
#include "funktionen.h"			// allgemeine Funktionen (Hardware und andere)
#include "speed.h"				// Geschwindigkeit anpassen
#include "ledc.h"				// LED-Controller
#include "i2cmaster.h"			// I2C Funktionen
#include "servo.h"
#include "adc.h"


// Watchdog Deaktivierung im Startcode. Notwendig, da Watchdog für Software-Reset (->Bootloader) verwendet wird
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
volatile unsigned char state = 0;		// Status-Variable für Interrupts und Main-Schleife

volatile unsigned int timer5_count = 0;			// zähler für timer5 (für isr, generelle Zeitintervalle)

char wlan_string[UART_MAXSTRLEN+1]="";  // globaler String zum Abspeichern des vom WLAN empfangen Strings in read_wlan

unsigned int speed = 0;					// globale variable für den Motor-Speed (reale Geschwindigkeit)
unsigned int speed_soll = 0;			// globale variable für den Motor-Speed - Vorgabe vom Controller
unsigned char richtung = RICHTUNG_VW;			// globlae variable für die Richtung (vorwärts = 1, rückwärts = 0)
unsigned char richtung_soll = RICHTUNG_VW;		// globlae variable für die Richtung - Vorgabe vom Controller (vorwärts = 1, rückwärts = 0)
unsigned char speedstep_korrektur = 0;			// Korrekturvariable, falls 8bit oder 9bit pwm statt 10bit verwendet wird
												// gibt an, um wieviele bit der speedwert geshiftet werden muss, damit er für die 10bit Verarbeitung passt (im timer3 interrupt)

unsigned char alivecount = 0;	// globlae Variable zum Zählen der Meldungen von der Gegenstelle
volatile char alivesecs = 0;	// globlae Variable die zählt, wie lange die Zählung geht -> man kann variable bestimmen, in welchem intervall geprüft wird

volatile unsigned char motor_reg = 1;	// Variable für Motor-Regelung ein/aus (auch für isr verwendet)
uint8_t motorerror = 0;					// Errorcode von Motorcontroller: 0 = kein Error

//ADC
volatile uint8_t adcchannel = 0;		// aktueller ADC channel 0-7
uint8_t adc_mask = 0;					// die benützten ADC Channels (siehe auch: adc_used in eedata.c), kein volatile, da sich der Wert im Betrieb nicht ändert
volatile uint8_t adcreadcount = 0;		// counter für Lesevorgänge pro ADC channel
unsigned int adcvalue[8];				// ADC Daten der 8 channels (Wert der letzten Auswertung)
volatile unsigned int adcvalue_work;	// ADC Wert der aktuellen Messungen: 4 Werte werden gelesen (addiert) und bei der Auswertung dann gemittelt

// frei verwendbar GPIOs
uint8_t  gpios_b;		// GPIOs Port B: 1: verwendet, 0: nicht verwendet
uint8_t  gpios_d;		//
uint8_t  gpios_e;		//
uint8_t  gpios_g;		//

// LokData im Flash
const char dev_swname[] PROGMEM = "lokbasis";     	// -> keine Änderung durch User -> flash
const char dev_swversion[] PROGMEM = "0.1";   		// -> keine Änderung durch User -> flash

//Strings im Flash für CMD-Rückmeldungen über WLAN
const char txtp_cmdend[] PROGMEM = ">";						// Befehlsende-Zeichen
const char txtp_cmdtrenn[] PROGMEM = ":";						// Options-Trennzeichen
const char txtp_errmotor[] PROGMEM = "<error:motor:";		// Motor-Error
const char txtp_sd[] PROGMEM = "<sd:";						// Speed-Rückmeldung
const char txtp_iam[] PROGMEM = "<iam:1:";					// Meldung mit Lok-Namen <iam:typ:name>
const char txtp_pong[] PROGMEM = "<pong>";					// Antwort für den ping Befe
const char txtp_default_lok_name[] PROGMEM = "Lok X";		// Standardwert für EEData Lokname
const char txtp_default_owner_name[] PROGMEM = "TheOwner";	// Standardwert für EEData Owner-Name
const char txtp_hwi[] PROGMEM = "<hwi:01> ";				// Antwort auf <hwget> 01 = Board UC02 (ATMega2561)
const char txtp_cmd_servoi[] PROGMEM = "<servoi:";			// Antwort auf <servoget>
const char txtp_cmd_ui[] PROGMEM = "<ui:";					// Rückmeldung Spannungswerte

// Befehle - Strings für auswertung, daher ohne spitze Klammern
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
const char txtp_cmd_nameset[] PROGMEM = "nameset:";
const char txtp_cmddata_start[] PROGMEM = "start:";
const char txtp_cmddata_add[] PROGMEM = "add:";
const char txtp_cmddata_end[] PROGMEM = "end:";
const char txtp_cmd_hwget[] PROGMEM = "hwget";
const char txtp_cmd_servoget[] PROGMEM = "servoget";
const char txtp_cmd_servoset[] PROGMEM = "servoset:";
const char txtp_cmd_gpioc[] PROGMEM = "gpioc:";


int main(void)
{
	char test[UART_MAXSTRLEN+1] = "";			// für string-Bearbeitungen (Rückmeldungen usw.)
	uint32_t loop_count = 0;					// Zähler für Hauptschleife
	uint32_t last_loop_count = 0;					// letztes gültiges Ergebnis des Zählers für Hauptschleife



	// ========================  Hardware Initialisierung  ========================================================

	init_eeprom();	// eeprom checken, ggf. vorbereiten

	init_uart(UART_NR_WLAN, UART_SETTING_WLAN);	// WLAN Daten Empfang aktivieren

	init_timer5(); 	// timer5 mit 244Hz (4ms)

	init_motorctrl();	// Ausgänge für Motorsteuerung, PWM

	#if defined( LEDC_PCA9622 ) || defined( LEDC_TLC59116 )
	i2c_init();
	ledcontrol_init(LEDC1);	// LED-Controller TODO: Funktion testen
	#endif	
	
	setbit(DDRD,PD6);	// Pin als Ausgang definieren - für Testsignal
	setbit(DDRD,PD7);	// Pin als Ausgang definieren - für Testsignal
	
	init_adc();
	initServo();


	init_gpios();		// frei verfügbare GPIOs als Ausgang definieren (nach Servos, ADC)
						//falls nichts Anderes konfiguriert wird, wird nur der komplette Port C als Ausgang definiert

	sei();	// Interrupts aufdrehen

	// ========================  Hardware Initialisierung abgeschlossen  ================================================


	wlan_puts("<MC start>");

	setbit(PORTD,PD6);	// TEST: auf HI

//-------------------------------------------------------------------------------------------------------------
//------                       H A U P T S C H L E I F E                                               --------
//-------------------------------------------------------------------------------------------------------------





	while (1)	// Hauptschleife (endlos)
	{
		loop_count++;	// Zähler für Hauptschleife

		PORT_TESTSIGNAL ^= (1<<TESTSIGNAL);	// Signale bei jedem Durchlauf togglen (oder 1x pro Sekunde weiter unten)


		if (alivesecs >= 3)	// //TODO lokdata // Prüfintervall = ALIVE_INTERVAL Sekunden! -> Lok stoppt nach ALIVE_INTERVAL sek herrenloser Fahrt
		{
			if (alivecount == 0)	// prüfen, ob in diesem Prüfintervall Meldungen der Gegenstelle einglangt sind
			{
				//alone = 1;

			}
				alivecount = 0;	// Zähler rücksetzen
				alivesecs = 0;
		}

		//Error von MotorController checken (passt für phb01 und Pololu 24v20) -> Error wird in motorerror gespeichert
		checkMotorStatus();  // TODO: Funktion testen


		if (state & STATE_5X_PRO_SEK)
		{
			set_speed();		// setzt die aktuelle Geschwindigkeit und checkt, ob speed-Änderungen "gesoftet" werden müssen

			cli();
			state &= ~STATE_5X_PRO_SEK;	// state-flag zurücksetzen
			sei();
		}

		if (state & STATE_1X_PRO_SEK)		// Meldung an Server bei == 1 / Steuergerät (ca. 1x pro Sekunde)
		{
			memset(test, 0, UART_MAXSTRLEN+1);	// string leeren
			strlcpy_P(test, txtp_sd, 5);	// Rückmeldung der Geschwindigkeit	//bei der Länge immer 0-Zeichen mitzählen!
			itoa(speed, test+4, 10);
			strlcat_P(test, txtp_cmdend, UART_MAXSTRLEN+1);	// will länge des kompletten "test" buffers+0
			wlan_puts(test);

			//TODO Test Servo
			if (servoValue[0] == CENTER) { servoValue[0] = 0; }
			else { servoValue[0] = CENTER; }
			if (servoValue[1] == CENTER) { servoValue[1] = 0; }
			else { servoValue[1] = CENTER; }


			/*
			// Test-Ausgabe MotorStatus
			memset(test, 0, UART_MAXSTRLEN+1);	// string leeren
			strlcpy_P(test, txtp_errmotor, 14);
			ltoa(motorerror, test+13, 10);
			strlcat_P(test, txtp_cmdend, UART_MAXSTRLEN+1); // will Länge des kompletten "test" buffers+0
			wlan_puts(test);
			*/


			/*
			ltoa(loop_count, test, 10);	// testweise Rückmeldung des loopcount -> später an handy
			log_puts_P("loopcount = ");
			log_puts(test);
			log_puts_P("\r\n");
			*/

			// Spannungsmeldung Schiene, Akku und ev. Motoren
			if (adc_mask != 0) { adc_msg_all(test); }	// alle ADC-Werte rückmelden

			last_loop_count = loop_count;	// ermittelten loopcount sichern
			loop_count = 0;	// loopcount sekündlich zurücksetzen -> ergibt loops/s
			cli();
			state &= ~STATE_1X_PRO_SEK;	// state-flag zurücksetzen
			sei();

		}

		if (state & STATE_ADC_CHECK) { check_adc(); }		// fertige ADC-Messungen auswerten

		// checken, ob WLAN-Daten vorhanden sind (wenn ja -> verarbeiten)
		#if defined( WLAN_UART_NR )	// WLAN_UART_NR = 1
			if (uart1_available() > 0) { check_wlan_cmd(); }
		#else // WLAN_UART_NR = 0
			if (uart0_available() > 0) { check_wlan_cmd(); }
		#endif	// WLAN_UART_NR

	}	// Ende Hauptschleife (endlos)

} // main ende




//-----------------------------------------------------------------------------------------
//----------------   Interrupts   ---------------------------------------------------------
//-----------------------------------------------------------------------------------------




//-------------------------------------------------------------------------------------------------------------
// isr für timer 5 output compare match A interrupt: 244 Hz = 4ms
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
		timer5_count = 0;
	}

}


