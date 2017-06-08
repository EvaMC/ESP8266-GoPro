/*
 * GoProControl and WebServer por Eva Mata Celaya
 * Version 6.0		06/06/2017
 *
 * Software  para microprocesador ESP8266 (ESP-12) 
 * Control remoto de una GoPro via WiFi y servidor web.
 * Probado con GoPro 3+ Silver
 *
 * Se necesita tener instalado:
 *  - Arduino IDE v1.8 (http://www.arduino.cc)
 *  - ESP8266 for Arduino (https://github.com/esp8266/Arduino) 
 *  - Librerias:
 *		+Conectar el puerto I2C
 *  		https://www.arduino.cc/en/reference/wire
 *		+Libreria generica de arduino:
 *			https://github.com/arduino/Arduino/blob/master/hardware/arduino/avr/cores/arduino/Arduino.h
 *		+Controla el WiFi del procesador, el cliente HTTP y el servidor web
 *			https://github.com/esp8266/Arduino/tree/master/libraries
 *		+Gestiona la pantalla:
 *			https://github.com/adafruit/Adafruit_SSD1306
 *		+Manejo de Strings:
 *			https://www.arduino.cc/en/Reference/StringObject
 */
 
/********************/
/* Librerias usadas */
/********************/
//Libreria para conectar el puerto I2C
#include <Wire.h> 
//Libreria generica de arduino, para leer pines analagicos, interrupciones,etc.
#include <Arduino.h> 
//Librerias necesarias para controlar el WiFi del procesador, el cliente HTTP y para el servidor web.
#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <WiFiClient.h> 

//Libreria para gestionar la pantalla
#include <Adafruit_SSD1306.h>
//Libreria para gestionar las interrupciones
extern "C" {
#include "user_interface.h"
}
//Libreria para el manejo de Strings.
#include <string.h>

/***********************/
/* Definiciones usadas */
/***********************/
#define SSID_NAME "Angelgopro"  	// SSID of your GoPro
#define WIFI_PASSWORD "Villares82"  // WiFi Password of you GoPro

//Peticiones implementadas:
#define HTTP_CODE_OK 				200
#define HTTP_CODE_ERROR_PASSWORD	403
#define HTTP_CODE_ERROR_CMDFAIL		410

#define GOPRO_HOST   "10.5.5.9"
#define GOPRO_PORT   80

#define GOPRO_ON     				"/bacpac/PW?t=%s&p=%%01"
#define GOPRO_OFF    				"/bacpac/PW?t=%s&p=%%00"
#define GOPRO_RECORD 				"/bacpac/SH?t=%s&p=%%01"
#define GOPRO_STOP   				"/bacpac/SH?t=%s&p=%%00"

#define GOPRO_VIDEO_MODE 			"/camera/CM?t=%s&p=%%00"
#define GOPRO_PHOTO_MODE 			"/camera/CM?t=%s&p=%%01"
#define GOPRO_BURST_MODE 			"/camera/CM?t=%s&p=%%02"
#define GOPRO_TIMELAPSE_MODE 		"/camera/CM?t=%s&p=%%03"

#define OLED_RESET LED_BUILTIN

/****************************************************/
/* Variables globales para el manejo de la pantalla */
/****************************************************/
Adafruit_SSD1306 oled(OLED_RESET);
bool screen = false;
String ip = " ";

/************************************************************/
/* Variables globales para mando de control remoto via WiFi */
/************************************************************/
ESP8266WiFiMulti WiFiMulti;
char request_str[45];		

typedef enum { VIDEO, PHOTO, BRUST, TIMELAPS, CONFIGURATION }Mode;
Mode mode = VIDEO;

typedef enum { CAPTURE, MODE, ON_OFF, NOBOTTON}Botton;
Botton botton = NOBOTTON;

bool captureStatus = false;
bool on_off = false;

os_timer_t myTimer;
int contCapture = 0;
int contMode = 0;

/******************************************************/
/* Variables globales para el manejo del servidor web */
/******************************************************/
String message = " ";
String request = String(10);
String p = String(10);
ESP8266WebServer server(80);

/*****************************************************************/
/* Funciones para el manejo del mando de control remoto via WiFi */
/*****************************************************************/
/*
 *	Funcion: 		readPulse(const IPAddress& ipAddress)
 *	Entrada: 		-
 *	Salida: 		-
 *	Descripcion: 	lee los valores del teclado conectado en la entrada analogica, determina cual de los dos botones se han pulsado, y asi
 *					como si la pulsacion ha sido larga o corta, para en funcion de esto, determinar que accion se quiere realizar, si 
 *					accionar o deterner el disparador, cambiar de modo, enceder o apagar la camara o si no se ha apretado ningun boton.
 */
void readPulse(){
   int reading;

   reading = analogRead(A0);    // Botones
   if (reading > 900)      // nada pulsado
   {
     if ((contCapture > 5) && (contCapture <30)){
       botton = CAPTURE;
     }else if (contMode >= 30){
       botton = ON_OFF;
     }else if ((contMode > 5) && (contMode <30)){
       botton = MODE;
     }else{
		botton = NOBOTTON;
		contCapture = 0;
		contMode = 0;  
     }
   }else if (reading < 400){
	   if (contCapture < 1000){
		   contCapture += 1;
		}
	 contMode = 0;  
   }else{
	   if (contMode < 1000){
		   contMode += 1;
		}
     contCapture = 0; 
   }
}
 /*
  *	Funcion: 		send_request(const char *request_fmt, int retries = 5, int delay_ms = 100)
  *	Entrada: 		
  *					const char *request_fmt => peticion
  *					int retries 			=> numero de intentos en caso de fallo
  *					int delay_ms			=> retraso entre intentos, en milisegundos
  *					
  *	Salida: 		devuelve true si se pudo realizar la peticion, devuelve false si fallo
  *	Descripcion: 	manda una peticion HTTP tantas veces como <retries> con un retraso <delay_ms>. Si retries<0,
  *					entonces lo intentara continuamente. Solo se imprime un error, pasado los 10 errores.
  * Ejemplo:
  * 				Formato de las petición HTTP
  *						http://10.5.5.9/param1/PARAM2?t=PASSWORD&p=%OPTION
  * 				En donde:
  * 					param1 es el lugar en donde va a tener lugar la acción en la camara o en el bacpac(WiFi)
  * 					param2 es el tipo de acción a realizar
  *						password es la contraseña de la GoPro
  *						option son los parámetros para dicha acción
  *					request_fmt es esta parte de la peticion: /param1/PARAM2?t=PASSWORD&p=%OPTION
  *					La password se rellena dentro de la función
  *   					request_fmt: "/bacpac/PW?t=%s&p=%%01"
  *   					WIFI_PASSWORD: Villares82
  *   					request_str: "/bacpac/PW?t=Villares82&p=%%01"
  */
bool send_request(const char *request_fmt, int retries = 5, int delay_ms = 100) {
    int httpCode, status;
    unsigned int time = 0;
    HTTPClient http;

    snprintf(request_str, 44, request_fmt, WIFI_PASSWORD);
    Serial.printf("Enviando: %s\n", request_str);

    while (((time < retries) && retries > 0) || (retries < 0)) {
        status = WiFiMulti.run();
        if ((status == WL_CONNECTED)) {
            http.begin(GOPRO_HOST, GOPRO_PORT, request_str);
            httpCode = http.GET();
            if (httpCode == HTTP_CODE_OK) {
                http.end();
                Serial.printf("OK\n");
                Serial.flush();
                return true;
            } else if (httpCode == HTTP_CODE_ERROR_PASSWORD) {
                Serial.printf("Password no es correcta\n");
                Serial.flush();
            } else if(httpCode == HTTP_CODE_ERROR_CMDFAIL){
				Serial.printf("El comando no es correcto\n");
                Serial.flush();
			}
        } else if (time % 10 == 0) {
            Serial.printf("No se ha conectado a GoPro WiFi: %d\n", status);
            Serial.flush();
        }
        time ++;
        delay(delay_ms);
    }

    http.end();
    Serial.printf("NOK: numero de intentos %d para mandar la petición: %s", retries, request_fmt);
    Serial.flush();
    return false;
}
/*
 *	Funcion: 		timerCallback(void *pArg)
 *	Entrada: 		-
 *	Salida: 		-
 *	Descripcion: 	se llama a la funcion donde se trata la interrupcion.
 */
void timerCallback(void *pArg){
	readPulse();
}

/*********************************************/
/* Funciones para el manejo del servidor web */
/*********************************************/
 /*
  *	Funcion: 		home()
  *	Entrada: 		-
  *	Salida: 		-
  *	Descripcion: 	genera la pagina principal del servidor web y gestiona los botones del mismo, realizando las acciones que estos representan.
  */
void home(){
	String message;
	message += 	"<html><head><title>GoPRO_Server</title>";
	message += 	"<style>body { background-color: #cccccc; font-family: Arial, Helvetica, Sans-Serif; Color: #000088; }</style></head>";
	message += 	"<body><h1><div align='center'>Welcome to your personal WebServer!!!";
	message +=	"<img src='http://www.abekislevitz.com/wp/wp-content/themes/abekislevitz2012/images/social/instagram-logo.png' width='' height='75'></h1>";
	
	//Boton ON
	message +=  "<form method='GET'>Switch camera ON-OFF  <br><input type='hidden' name='request' value='PW'><input type='hidden' name='p' value='01'><input type='submit' value='ON'</br></form>";
	//Boton OFF
	message +=  "<form method='GET'><input type='hidden' name='request' value='PW'><input type='hidden' name='p' value='00'><input type='submit' value='OFF'></form>";
	//Boton REC
	message +=  "<form method='GET'>Start or stop recording    <br><input type='hidden' name='request' value='SH'><input type='hidden' name='p' value='00'><input type='submit' value='REC'</br></form>";
	//Boton STOP REC
	message +=  "<form method='GET'><input type='hidden' name='request' value='SH'><input type='hidden' name='p' value='01'><input type='submit' value='STOP REC'></form>";

	//Boton CHANGE MODE
	message +=  "<form method='GET'>Choose mode <br><input type='hidden' name='request' value='CM'><select name='p'</br>>";
	message +=  "<option value='00'>Video</option><option value='01'>Photo</option><option value='02'>Brust</option><option value='03'>Timelaps</option>";
	message +=  "</select><input type='submit' value='Choose Mode'></form> ";

	//Boton VER CONTENIDO
	message +=  "<form action='http://10.5.5.9:8080/videos/DCIM/100GOPRO/' method='GET'><input type='submit' value='View content' /></form>";
	//Boton LIVE STREAMING
	message +=  "<form action='http://10.5.5.9:8080/live/' method='GET'><input type='submit' value='Live streaming' /></form>";
	message +=  "</body></html>";

	request = server.arg("request");
	p = server.arg("p");
   
	if(request=="PW"){
		if(p=="00"){
			if(on_off){
				if(send_request(GOPRO_OFF, -1, 1000))
					Serial.println("La camara se apagara");
				on_off = ! on_off;
				mode = VIDEO;
			}else{
				Serial.println("La camara ya esta apagada");
			}
			printBye();
		}else{
			if(!on_off){
				if(send_request(GOPRO_ON, -1, 1000))
					Serial.println("La camara se encendera");
				on_off = ! on_off;
		  mode = VIDEO;
			}else{
				Serial.println("La camara ya esta encendida");
			}
			printWelcome();
		}
	}else if(request=="SH"){
		if(p=="00"){
		if((mode == PHOTO) || (mode == BRUST)){
			if(send_request(GOPRO_RECORD))
				Serial.printf("REC...\n");
			if(send_request(GOPRO_STOP))
				Serial.printf("STOP...\n");  
			printCapture();
		}else{
			  if(!captureStatus){
				  if(send_request(GOPRO_RECORD))
					  Serial.printf("REC...\n");
					captureStatus=!captureStatus;
			printRec();
			  }else{
				  Serial.println("Ya esta grabando");
			  }
		}
		}else{
			if(captureStatus){
				if(send_request(GOPRO_STOP))
					Serial.printf("STOP...\n");
				captureStatus=!captureStatus;
		  printStopRec();
			}else{
				Serial.println("Ya no esta grabando");
			}
		}
	}else if(request=="CM"){
		if(p=="00"){
			if(send_request(GOPRO_VIDEO_MODE))
				mode = VIDEO;
		}else if(p=="01"){
			if(send_request(GOPRO_PHOTO_MODE))
				mode = PHOTO;
		}else if(p=="02"){
			if(send_request(GOPRO_BURST_MODE))
				mode = BRUST;
		}else{
			if(send_request(GOPRO_TIMELAPSE_MODE))
				mode = TIMELAPS;
		}
		printWaitingAction();
	}
	server.send(200, "text/html", message);
}
/*
 *	Funcion: 		handleNotFound()
 *	Entrada: 		-
 *	Salida: 		-
 *	Descripcion: 	en caso de que alguien intente entrar en una pagina que no exista en el servidor web, genera el error.
 */
void handleNotFound(){
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET)?"GET":"POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  
  for (uint8_t i = 0; i < server.args(); i++)
  {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  
  server.send(404, "text/plain", message);
}

/*******************************************/
/* Funciones para el manejo de la pantalla */
/*******************************************/
/*
 *	Funcion: 		ipAddressToString(const IPAddress& ipAddress)
 *	Entrada: 		recibe una IPAddress ip 
 *	Salida: 		devuelve la ip en formato string
 *	Descripcion: 	realiza el casting a string y concatenacion de los elementos de una ip
 */
String ipAddressToString(const IPAddress& ipAddress){
  return String(ipAddress[0]) + String(".") +\
  String(ipAddress[1]) + String(".") +\
  String(ipAddress[2]) + String(".") +\
  String(ipAddress[3])  ; 
}
/*
 *	Funciones:printRec()
 *	Entradas: 		-
 *	Salidas: 		-
 *	Descripcion: imprime el mensaje en la pantalla OLED,   
 *          para informar de que esta comenzando a grabar un 
 *          video o capturar imágenes si estamos en modo 
 *          temporizador.
 */
void printRec(void){
	oled.clearDisplay();
	oled.clearDisplay();
	oled.setTextSize(2);
	oled.setCursor(45,25);
	oled.println("REC");
	oled.setTextSize(1);
	oled.setCursor(0,50);
	if(mode==PHOTO){
		oled.println("Mode => PHOTO");
	}else if(mode == VIDEO){
		oled.println("Mode => VIDEO"); 
	}else if(mode == BRUST){
		oled.println("Mode => BRUST");   
	}else if(mode == TIMELAPS){
		oled.println("Mode => TIMELAPS");    
	}else{
		oled.println("Mode => CONFIGURATION");      
	}
	oled.display();
  delay(2000);
}
/*
 *	Funciones: printStopRec ()
 *	Entradas: 		-
 *	Salidas: 		-
 *	Descripcion: imprime el mensaje en la pantalla OLED,
 *        para informar de que esta finalizando a grabar un  
 *        video o capturar imágenes si estamos en modo 
 *        temporizador.
 */
void printStopRec(void){
	oled.clearDisplay();
  oled.clearDisplay();
	oled.setTextSize(2);
	oled.setCursor(15,25);
	oled.println("STOP REC");
	oled.setTextSize(1);
	oled.setCursor(0,50);
	if(mode==PHOTO){
		oled.println("Mode => PHOTO");
	}else if(mode == VIDEO){
		oled.println("Mode => VIDEO"); 
	}else if(mode == BRUST){
		oled.println("Mode => BRUST");   
	}else if(mode == TIMELAPS){
		oled.println("Mode => TIMELAPS");    
	}else{
		oled.println("Mode => CONFIGURATION");      
	}
	oled.display();
	screen = !screen;
  delay(2000);
}
/*
 *Funciones: printCapture ()
 *Entradas: 		-
 *Salidas: 		-
 *Descripcion: imprime el mensaje en la pantalla 
 *           OLED, para informar de que se esta tomando una 
 *           foto o se está capturando las imagenes en modo 
 *           ráfaga.
 */
void printCapture(void){
	oled.clearDisplay();
  oled.clearDisplay();
	oled.setTextSize(2);
	oled.setCursor(15,25);
	oled.println("CAPTURING...");
	oled.setTextSize(1);
	oled.setCursor(0,50);
	if(mode==PHOTO){
		oled.println("Mode => PHOTO");
	}else if(mode == VIDEO){
		oled.println("Mode => VIDEO"); 
	}else if(mode == BRUST){
		oled.println("Mode => BRUST");   
	}else if(mode == TIMELAPS){
		oled.println("Mode => TIMELAPS");    
	}else{
		oled.println("Mode => CONFIGURATION");      
	}
	oled.display();
	screen = !screen;
  delay(2000);
}
/*
 *Funciones: printWaitingAction ()
 *Entradas: 		-
 *Salidas: 		-
 *Descripcion: imprime el mensaje en la pantalla OLED de que
 *        se está disponible y esperando a que se realice una
 *        acción.
 */
void printWaitingAction(void){
	oled.clearDisplay();
  oled.clearDisplay();
	oled.setTextSize(1);
	oled.setCursor(0,0);
	oled.println("Action Cam Controler");
  oled.setCursor(0,10);
  oled.println("WebServer:"+ip);
	oled.setCursor(2,0);
	oled.setTextSize(2);
	oled.setCursor(15,25);
	oled.println("WAITING...");
	oled.setTextSize(1);
	oled.setCursor(0,50);
	if(mode==PHOTO){
		oled.println("Mode => PHOTO");
	}else if(mode == VIDEO){
		oled.println("Mode => VIDEO"); 
	}else if(mode == BRUST){
		oled.println("Mode => BRUST");   
	}else if(mode == TIMELAPS){
		oled.println("Mode => TIMELAPS");    
	}else{
		oled.println("Mode => CONFIGURATION");      
	}
	oled.display();
}
/*
 *Funciones: printBye ()
 *Entradas: 		-
 *Salidas: 		-
 *Descripcion: imprime el mensaje en la pantalla OLED de que 
 *             se está apagando la cámara.
 */
void printBye(void){
	oled.clearDisplay();
  oled.clearDisplay();
	oled.setTextSize(1);
	oled.setCursor(0,0);
	oled.println("Action Cam Controler");
  oled.setCursor(0,10);
  oled.println("WebServer:"+ip);
	oled.setTextSize(2);
	oled.setCursor(15,25);
	oled.println("BYE,BYE!");
	oled.display();
	screen = true;
  delay(3000);
  oled.clearDisplay();
  oled.clearDisplay();
  oled.display();
}
/*
 *Funciones: printWelcome ()
 *Entradas: 		-
 *Salidas: 		-
 *Descripcion: imprime el mensaje en la pantalla OLED de que 
 *             se está encendiendo la cámara.
 */
void printWelcome(void){
	oled.clearDisplay();
  oled.clearDisplay();
	oled.setTextSize(1);
	oled.setCursor(0,0);
	oled.println("Action Cam Controler");
	oled.setCursor(0,10);
  oled.println("WebServer:"+ip);
	oled.setTextSize(2);
	oled.setCursor(15,25);
	oled.println("WELCOME!");
	oled.display();
	screen = !screen;
  delay(3000);
}

/*
*Funciones: setup()
*Entradas: 		-
*Salidas: 		-
*Descripcion: funcion de configuracion, solo se ejecuta una unica vez.
*/
void setup(){
  Serial.begin(115200);
  Serial.println("Comienzo...");
  
  Serial.printf("Intentando conectar a la red %s\n", SSID_NAME);
  WiFiMulti.addAP(SSID_NAME, WIFI_PASSWORD);
  int status = WiFiMulti.run();
  if (status == WL_CONNECTED) {
	  Serial.printf("CONECTADO!\n");
  } else {
	  Serial.printf("No conectado: %d\n", status);
  }
  WiFiMulti.run();
  send_request(GOPRO_ON, -1, 1000);  
  on_off=!on_off;
  
  os_timer_setfn(&myTimer, timerCallback, NULL);
  os_timer_arm(&myTimer, 10, true);
  
  /*Setup del web server-->Set page handler functions */
  server.on("/", home);
  server.onNotFound(handleNotFound);
  
  server.begin();
  Serial.println();
  Serial.print("http://");
  Serial.println(WiFi.localIP());
  ip = ipAddressToString(WiFi.localIP());
  Serial.println("HTTP server started");
  
  //Para saber la posicion de memoria ejecutar i2c_scanner: http://playground.arduino.cc/Main/I2cScanner
  oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);

  // Clear the buffer.
  oled.clearDisplay();
  oled.clearDisplay();
  oled.display();

  oled.setRotation(0);      /*Nos da el giro de pantalla: puede tener 4 posiciones*/
  oled.setTextSize(1);
  oled.setTextColor(WHITE);
  oled.setCursor(0,0);
  oled.println("Action Cam Controler");
  oled.setCursor(0,10);
  oled.println("WebServer:"+ip);
  oled.setTextSize(2);
  oled.setCursor(15,25);
  oled.println("WELCOME!");
  oled.display();
  delay(5000);
}
/*
*Funciones: loop()
*Entradas: 		-
*Salidas: 		-
*Descripcion: funcion principal, que se ejecuta de forma ciclica de forma indefinida.
*/
void loop() {
	server.handleClient();
	switch(botton)
	{
		case NOBOTTON:
			if(!screen){
				printWaitingAction();
			  screen = !screen;
			}
			break;
		case CAPTURE:
			if((mode == PHOTO) || (mode == BRUST)){
				if(send_request(GOPRO_RECORD))
					Serial.printf("REC...\n");
				if(send_request(GOPRO_STOP))
					Serial.printf("STOP...\n");  
				printCapture();
			}else{
				if(!captureStatus){
					if(send_request(GOPRO_RECORD))
						Serial.printf("REC...\n");
					captureStatus=!captureStatus;
					printRec();
				}else{
					if(send_request(GOPRO_STOP))
						Serial.printf("STOP...\n");
					captureStatus=!captureStatus;
					printStopRec();
				}
			}
			botton = NOBOTTON;
			contCapture = 0;
			contMode = 0;  
			break;
		case ON_OFF:
			if(on_off){
				if(send_request(GOPRO_OFF, -1, 1000))
					Serial.println("La camara se apagara");
				on_off = ! on_off;
        mode = VIDEO;
				printBye();
			}else{	
				if(send_request(GOPRO_ON, -1, 1000))
					Serial.println("La camara se encendera");
				on_off = ! on_off;
        mode = VIDEO;
				printWelcome();
			}
			botton = NOBOTTON;
			contCapture = 0;
			contMode = 0;  
			break;
		default:
			if (mode == VIDEO){
				if(send_request(GOPRO_PHOTO_MODE)){
					mode = PHOTO;
				}else{
					mode = VIDEO;
				}
				printWaitingAction();
			}else if (mode == PHOTO){
				if(send_request(GOPRO_BURST_MODE)){
				  mode = BRUST;
				}else{
					mode = PHOTO;
				}
				printWaitingAction();
			}else if (mode == BRUST){
				if(send_request(GOPRO_TIMELAPSE_MODE)){
					mode = TIMELAPS;
				}else{
					mode = BRUST;
				}
				printWaitingAction();
			}else if (mode == TIMELAPS) {
				Serial.println("A desarrollar el cambio del modo temporizador a la configuración...");
				if(send_request(GOPRO_VIDEO_MODE)){
					mode = VIDEO;
				}else{
					mode = TIMELAPS;
				}
				mode = VIDEO;
				printWaitingAction();
			}else {//Modo configuración
				Serial.println("Modo configuracion no implementadi");
				mode = VIDEO;
			printWaitingAction();
			}
			botton = NOBOTTON;
			contCapture = 0;
			contMode = 0;  
			break;
	}
}
