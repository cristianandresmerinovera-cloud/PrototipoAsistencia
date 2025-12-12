#include <Arduino.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <Adafruit_Fingerprint.h>
#include <Preferences.h>
#include <Keypad.h>
#include <HardwareSerial.h>
#include <time.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// Dirección I2C y configuración de pines personalizados
LiquidCrystal_I2C lcd(0x27, 16, 2);  // Dirección 0x27, pantalla de 16x2

Preferences prefs;
// Cache de cédulas registradas en RAM para acceso rápido
struct Registro {
  String cedula;
  int idHuella;
};
Registro cache[127]; // máximo 127 huellas
int totalCache = 0;

// Función para abreviar asignaturas
String abreviarAsignatura(String asignatura) {
  // Palabras a ignorar
  String ignorar[] = {"DE","DEL","Y","LA","LAS","EN","PARA","UN","UNA","EL"};

  if (asignatura.length() <= 16) return asignatura; // si cabe, no recortar

  String resultado = "";
  int maxLineLength = 16;

  // Separar palabras
  int start = 0;
  int end = asignatura.indexOf(' ');

  while (end != -1) {
    String palabra = asignatura.substring(start, end);
    palabra.trim();

    bool ignorarPalabra = false;
    for (int i = 0; i < sizeof(ignorar)/sizeof(ignorar[0]); i++) {
      if (palabra.equalsIgnoreCase(ignorar[i])) {
        ignorarPalabra = true;
        break;
      }
    }

    if (!ignorarPalabra) {
      // Solo recortar si palabra > 8, sino tomar completa
      if (palabra.length() > 8) palabra = palabra.substring(0, 6); // recorta menos agresivo
      if (resultado.length() + palabra.length() + 1 <= maxLineLength) {
        if (resultado.length() > 0) resultado += " ";
        resultado += palabra;
      }
    }

    start = end + 1;
    end = asignatura.indexOf(' ', start);
  }

  // Última palabra
  String ultima = asignatura.substring(start);
  ultima.trim();
  bool ignorarUltima = false;
  for (int i = 0; i < sizeof(ignorar)/sizeof(ignorar[0]); i++) {
    if (ultima.equalsIgnoreCase(ignorar[i])) ignorarUltima = true;
  }
  if (!ignorarUltima) {
    if (ultima.length() > 8) ultima = ultima.substring(0, 6); // recorta menos agresivo
    if (resultado.length() + ultima.length() + 1 <= maxLineLength) {
      if (resultado.length() > 0) resultado += " ";
      resultado += ultima;
    }
  }

  return resultado;
}



const unsigned long TIEMPO_ESPERA = 10000; // 10 segundos

// -------------------- WiFi --------------------

const char* ssid = "Internet_UNL";
const char* password = "UNL1859WiFi";
// ----------------- FIREBASE -------------------
#define API_KEY "AIzaSyBByttCwTn1hMwo9X5P2gbsYlSq9lGa7t0"
#define PROJECT_ID "escaner-qr-d7d65"
#define USER_EMAIL "cristian.a.merino@unl.edu.ec"
#define USER_PASSWORD "10112002"


String adminEmail = "admin@laboratorio.com";
String PIN_CORRECTO = "020302";
int intentosFallidos = 0;
unsigned long tiempoBloqueo = 0;
bool bloqueado = false;


const int MAX_INTENTOS = 3;
const unsigned long TIEMPO_BLOQUEO_MS = 15000; // 15 segundos bloqueado


String tipoAsistencia = "clase";       // "clase" o "practica"
String preparatorioGlobal = "";   
     // "sí" o "no"
String modo = "verificacion";  // Modo iniciial

String grupoActual = "";               // editable por estudiante
String horaAnterior = "";              // para detectar cambio de hora

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
FirebaseData loginFbdo;   

// ------------------ Sensor de Huellas ------------------
HardwareSerial mySerial(2);  // UART2
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);

// ------------------ Teclado 4x4 -----------------------
const byte ROWS = 4; // Cuatro filas
const byte COLS = 4; // Cuatro columnas

char keys[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

// Filas del teclado
byte rowPins[ROWS] = {19, 18, 5, 22};   

// Columnas del teclado (con pines corregidos)
byte colPins[COLS] = {23, 4, 12, 13};  

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

unsigned long tiempoAnterior = 0;
const unsigned long intervaloLectura = 5000; // 5 segundos

int idHuella = 0;  // ID de huella ingresado por el usuario

// -------------------- Estructura y cache --------------------
struct Horario {
  String horaIni;
  String horaFin;
  String asignatura;
  String profesor;
  String ciclo;
};

Horario cacheHorarios[100]; // Ajustar según la cantidad de horarios
int totalHorarios = 0;
int ultimaLecturaDia = -1; // Para controlar la lectura diaria

String leerPin() {
  String pin = "";

  lcd.clear();
  lcd.print("Ingrese PIN:");
  lcd.setCursor(0, 1);

  while (pin.length() < 6) {
    char tecla = keypad.getKey();

    if (tecla && isDigit(tecla)) {

      // ===== MOSTRAR EN LCD =====
      lcd.print("*");

      // ===== MOSTRAR EN SERIAL EL DIGITO REAL =====
      Serial.print("Tecla detectada: ");
      Serial.println(tecla);

      // ===== AGREGAR AL PIN =====
      pin += tecla;
    }
  }

  Serial.print("PIN completo: ");
  Serial.println(pin);

  return pin;
}



// ----------------- FUNCIÓN lcdMensaje ------------------
void lcdMensaje(String linea1, String linea2 = "", int pausa = 250) {
  lcd.clear();

  if (linea1.length() <= 16) {
    lcd.setCursor(0, 0);
    lcd.print(linea1);
    delay(1000);
  } else {
    for (int i = 0; i <= linea1.length() - 16; i++) {
      lcd.setCursor(0, 0);
      lcd.print(linea1.substring(i, i + 16));
      delay(pausa);
    }
    delay(500);
  }

  if (linea2 != "") {
    if (linea2.length() <= 16) {
      lcd.setCursor(0, 1);
      lcd.print(linea2);
      delay(1000);
    } else {
      for (int i = 0; i <= linea2.length() - 16; i++) {
        lcd.setCursor(0, 1);
        lcd.print(linea2.substring(i, i + 16));
        delay(pausa);
      }
      delay(500);
    }
  }
}

void lcdScrollMensaje(String mensaje, int fila = 0, int pausa = 300) {
  lcd.clear();
  if (mensaje.length() <= 16) {
    lcd.setCursor(0, fila);
    lcd.print(mensaje);
    delay(2000);
  } else {
    for (int i = 0; i <= mensaje.length() - 16; i++) {
      lcd.setCursor(0, fila);
      lcd.print(mensaje.substring(i, i + 16));
      delay(pausa);
    }
    delay(1000);  // Pausa final tras el scroll completo
  }
}
// --- Cargar cache desde Preferences ---
void cargarCache() {
  prefs.begin("cacheHuella", true); // lectura
  totalCache = 0;
  for (int i = 1; i <= 127; i++) {
    String key = "id_" + String(i);
    String cedula = prefs.getString(key.c_str(), "");
    if (cedula != "") {
      cache[totalCache].cedula = cedula;
      cache[totalCache].idHuella = i;
      totalCache++;
    }
  }
  prefs.end();
}
void seleccionarModoInicial() {
  lcd.clear();
  lcdMensaje("A: Registro", "B: Verificacion");

  unsigned long tiempoInicio = millis();
  char key = NO_KEY;

  while (millis() - tiempoInicio < TIEMPO_ESPERA) {
    key = keypad.getKey();
    if (key == 'A' || key == 'a') {
      modo = "registro";
      lcdMensaje("Modo: REGISTRO");
      delay(1000);
      return;
    } 
    else if (key == 'B' || key == 'b') {
      modo = "verificacion";
      lcdMensaje("Modo: VERIFICACION");
      delay(1000);
      return;
    }
    delay(50); // pequeño retardo para no saturar el loop
  }

  // Si se acaba el tiempo, entrar a verificación por defecto
  modo = "verificacion";
  lcdMensaje("Tiempo agotado", "Modo: VERIFICACION");
  delay(1000);
}

// --- Guardar cédula en Preferences ---
void guardarCache(String cedula, int idHuella) {
  prefs.begin("cacheHuella", false); // escritura
  String key = "id_" + String(idHuella);
  prefs.putString(key.c_str(), cedula);
  prefs.end();
}

// --- Función para pedir cédula ---
String ingresarCedula() {
  String cedula = "";
  char key;

  lcd.clear();
  lcdMensaje("Ingresa cedula:" );

  while (true) {
    key = keypad.getKey();

    // Cancelar
    if (key == '#') {
      lcdMensaje("Operacion cancelada");
      delay(1500);
      return ""; // Indica cancelación
    }

    // Finalizar con *
    if (key == '*') {
      if (cedula.length() != 10) {
        lcdMensaje("Cedula incompleta", "10 digitos requeridos");
        delay(1500);
        lcd.clear();
        lcdMensaje("Ingresa cedula:", "Pulsa * para OK");
        continue;
      }
      break;
    }

    // Números
    if (key >= '0' && key <= '9' && cedula.length() < 10) {
      cedula += key;
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Digitando cedula");
      lcd.setCursor(0, 1);
      lcd.print(cedula);
    }
  }

  return cedula;
}

// --- Buscar cédula en cache ---
int buscarCache(String cedula) {
  for (int i = 0; i < totalCache; i++) {
    if (cache[i].cedula == cedula) return cache[i].idHuella;
  }
  return -1; // no existe
}

// --- Obtener ID libre ---
int obtenerIdLibre() {
  bool usado[128] = { false };
  for (int i = 0; i < totalCache; i++) {
    if (cache[i].idHuella >= 1 && cache[i].idHuella <= 127)
      usado[cache[i].idHuella] = true;
  }
  for (int i = 1; i <= 127; i++) {
    if (!usado[i]) return i;
  }
  return -1; // sin espacio
}
void actualizarHorarios() {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  String dia = diaSemanaStr(t->tm_wday);

  lcdMensaje("Config. sistema", "Espere...");
  Serial.println("Actualizando horarios del día: " + dia);

  totalHorarios = 0;

  // Solo de 13 a 21 horas
  for (int horaIni = 13; horaIni <= 21; horaIni++) {
    for (int duracion = 1; duracion <= 4; duracion++) {
      int horaFin = horaIni + duracion;
      if (horaFin > 22) continue;  // límite 21 horas

      char horaIniStr[6], horaFinStr[6];
      sprintf(horaIniStr, "%02d:00", horaIni);
      sprintf(horaFinStr, "%02d:00", horaFin);

      String docID = dia + "_" + String(horaIniStr) + "_" + String(horaFinStr);
      String path = "horario_laboratorio/" + docID;

      if (Firebase.Firestore.getDocument(&fbdo, PROJECT_ID, "", path.c_str())) {
        FirebaseJson horario;
        horario.setJsonData(fbdo.payload());
        FirebaseJsonData campo;

        Horario h;
        horario.get(campo, "fields/asignatura/stringValue"); h.asignatura = campo.stringValue;
        horario.get(campo, "fields/profesor/stringValue"); h.profesor = campo.stringValue;
        horario.get(campo, "fields/ciclo/stringValue"); h.ciclo = campo.stringValue;

        h.horaIni = String(horaIniStr);
        h.horaFin = String(horaFinStr);

        if (totalHorarios < 100) cacheHorarios[totalHorarios++] = h;

        Serial.println(String(h.horaIni) + "-" + String(h.horaFin) + ": " + h.asignatura + " | " + h.profesor + " | " + h.ciclo);
      }
    }
  }

  ultimaLecturaDia = t->tm_mday;
  Serial.println("Actualización de horarios completada");
}

void setup() {
  Serial.begin(115200);  // (puedes dejarlo activo si deseas depurar)

  // Inicializar LCD (I2C con SDA=26, SCL=25)
  Wire.begin(26, 25);  
  lcd.begin(16, 2);
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);

  //pinMode(BOTON_REGISTRO, INPUT_PULLUP);
  //pinMode(BOTON_VERIFICACION, INPUT_PULLUP);

  // Sensor de huellas
  mySerial.begin(57600, SERIAL_8N1, 16, 17); // UART2: RX=16, TX=17

  // Conexión WiFi
  WiFi.begin(ssid, password);
  lcdMensaje("Conectando WiFi", "");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    lcd.setCursor(0, 1);
    lcd.print(".");
  }
  lcdMensaje("WiFi conectado", "");

  // Sincronizar hora con NTP
  configTime(-5 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  // lcdMensaje("Sincronizando", "hora NTP...");
  time_t now = time(nullptr);
  while (now < 8 * 3600 * 2) {
    delay(100);
    now = time(nullptr);
  }

  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char horaTexto[17];
  strftime(horaTexto, sizeof(horaTexto), "%H:%M %d/%m", &timeinfo);
  Serial.println("Hora sincronizada: " + String(horaTexto));

  // Firebase
  config.api_key = API_KEY;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // Verificar sensor de huellas
  finger.begin(57600);
  if (finger.verifyPassword()) {
    lcdMensaje("Sensor huella", "OK");
  } else {
    lcdMensaje("Error sensor huella");
    while (true);  // Detener el sistema
  }
  seleccionarModoInicial();  // <<--- Aquí se llama la función

  // -------------------- Leer horarios al encendido --------------------
  actualizarHorarios();
  cargarCache(); 
  imprimirCache();
}

void seleccionarTipoAsistencia() {
  lcdMensaje("Selecciona", "tipo de asistencia: ");
  delay(3000);
  lcdMensaje("1. Clase", "2. Practica");

  char key = keypad.getKey();
  while (key == NO_KEY) {
    key = keypad.getKey();  // Esperar hasta que se presione una tecla
  }

  // Cancelación en menú principal
  if (key == '#') {
    lcdMensaje("Operacion cancelada");
    delay(1000);
    seleccionarTipoAsistencia(); // vuelve al inicio
    return;
  }

  tipoAsistencia = (key == '2') ? "practica" : "clase";  // Definir tipo de asistencia

  if (tipoAsistencia == "practica") {
    lcdMensaje("Es preparatorio?");
    delay(2500);
    lcdMensaje("1. Si", "2. No");

    key = keypad.getKey();
    while (key == NO_KEY) {
      key = keypad.getKey();  // Esperar hasta que se presione una tecla
    }

    // Cancelación en pregunta de preparatorio
    if (key == '#') {
      lcdMensaje("Operacion cancelada");
      delay(1000);
      seleccionarTipoAsistencia(); // vuelve al inicio
      return;
    }

    preparatorioGlobal = (key == '1') ? "si" : "no";  // Definir si es preparatorio
  } else {
    preparatorioGlobal = "";
  }

  //lcdMensaje("Tipo: " + tipoAsistencia);
  //delay(1000);

  if (tipoAsistencia == "practica") {
    //lcdMensaje("Preparatorio: " + preparatorioGlobal);
    //delay(2000);
  }
}

bool pinYaValidado = false;

String pedirPin() {
  lcdMensaje("Modo registro", "Ingrese PIN:");
  String pin = "";

  while (true) {
    char key = keypad.getKey();

    if (key >= '0' && key <= '9') {
      if (pin.length() < 4) {
        pin += key;
        lcdMensaje("PIN:", String(pin.length(), '*'));
      }
    }

    if (key == '#') {
      lcdMensaje("Cancelado");
      delay(1000);
      return "";
    }

    if (key == '*') {  // ENTER
      return pin;
    }
  }
}

bool verificarPin() {
  if (bloqueado) {
    if (millis() - tiempoBloqueo < TIEMPO_BLOQUEO_MS) {
      lcdMensaje("PIN bloqueado", "Espere...");
      return false;
    } else {
      bloqueado = false;
      intentosFallidos = 0;   // resetear intentos
    }
  }

  String pinIngresado = leerPin();
  

  String pinCorrecto = PIN_CORRECTO;  // <-- cámbialo por tu PIN real

Serial.print("PIN ingresado >");
Serial.print(pinIngresado);
Serial.println("<");

Serial.print("PIN correcto   >");
Serial.print(pinCorrecto);
Serial.println("<");
  if (pinIngresado == pinCorrecto) {
    intentosFallidos = 0;
    lcdMensaje("PIN Correcto");
    return true;
  } else {
    intentosFallidos++;
    lcdMensaje("PIN Incorrecto");

    if (intentosFallidos >= MAX_INTENTOS) {
      bloqueado = true;
      tiempoBloqueo = millis();
      lcdMensaje("Demasiados", "Intentos");
      delay(1000);
      lcdMensaje("Bloqueado 15s");
    }

    return false;
  }
}


bool validarPinFirebase(String pinIngresado) {

  // Validar 6 dígitos
  if (pinIngresado.length() != 6) {
    Serial.println("El PIN debe tener 6 dígitos");
    return false;
  }

  FirebaseAuth authTemp;        // auth temporal
  FirebaseConfig configTemp;    // config temporal
  FirebaseData fbdoTemp;

  configTemp.api_key = API_KEY;
  authTemp.user.email = adminEmail;
  authTemp.user.password = pinIngresado;

  Firebase.begin(&configTemp, &authTemp);

  Serial.println("Intentando login...");

  if (Firebase.ready()) {
    Serial.println("PIN CORRECTO");
    return true;
  } else {
    Serial.println("PIN INCORRECTO");
   Serial.println(loginFbdo.errorReason());
    return false;
  }
}


bool accesoModoRegistro() {

  // Si ya lo validó antes, no volver a pedirlo
  if (pinYaValidado) {
    return true;
  }

  // Si está bloqueado, verificar si ya pasó el tiempo
  if (bloqueado) {
    if (millis() - tiempoBloqueo < TIEMPO_BLOQUEO_MS) {
      lcdMensaje("PIN bloqueado", "Espere...");
      return false;
    } else {
      bloqueado = false;
      intentosFallidos = 0;
    }
  }

  while (intentosFallidos < MAX_INTENTOS) {

    String pinIngresado = leerPin();
    String pinCorrecto = "020302";

    if (pinIngresado == pinCorrecto) {
      intentosFallidos = 0;
      pinYaValidado = true;  // ← La clave: marcar que ya no se vuelve a pedir
      lcdMensaje("PIN Correcto");
      return true;
    }

    intentosFallidos++;
    lcdMensaje("PIN Incorrecto");

    if (intentosFallidos >= MAX_INTENTOS) {
      bloqueado = true;
      tiempoBloqueo = millis();
      lcdMensaje("Demasiados Intentos", "Bloqueado");
      return false;
    }

    lcdMensaje("Intentelo de", "nuevo");
    delay(500);
  }

  return false;
}



void loop() {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  if (t->tm_mday != ultimaLecturaDia && t->tm_hour == 13) {
    actualizarHorarios();
  }

  unsigned long tiempoActual = millis();
  struct tm* timeinfo = localtime(&now);
  char horaRedondeada[6];
  sprintf(horaRedondeada, "%02d:00", timeinfo->tm_hour);

  if (modo == "verificacion" && horaAnterior != String(horaRedondeada)) {
    horaAnterior = String(horaRedondeada);
    seleccionarTipoAsistencia();  // Pregunta solo en verificación
  }

  if (tiempoActual - tiempoAnterior >= intervaloLectura) {
    tiempoAnterior = tiempoActual;



if (modo == "registro") {

  if (!accesoModoRegistro()) {
    modo = "verificacion";  
    return;
  }

  // Si el PIN está ok
  lcdMensaje("REGISTRO");
  registrarHuellaSP32();
}



else if (modo == "verificacion") {
  lcdMensaje("VERIFICACION", "Pon tu dedo...", 100);

  while (true) {
    char key = keypad.getKey();
    if (key == '#') {
      lcdMensaje("Operacion cancelada");
      delay(1000);
      seleccionarTipoAsistencia(); // vuelve al inicio
      return;
    }

    if (finger.getImage() == FINGERPRINT_OK) {
      if (verificarHuella()) {
        // asistencia marcada
      } else {
      }
      break; // salir del while después de procesar la huella
    }

    delay(100); // evita saturar el loop
  }
}
}
}

int ingresarIdHuella() {
  String idStr = "";
  char key;
  unsigned long ultimoScroll = 0;
  const unsigned long intervaloScroll = 3000;
  bool mostrandoScroll = false;

  ultimoScroll = millis();
  mostrandoScroll = true;

  while (true) {
    key = keypad.getKey();

    // CANCELAR con #
    if (key == '#') {
      lcdMensaje("Operacion cancelada");
      delay(1500);
      return -1; // Indica que se canceló
    }

    // Finalizar con *
    if (key == '*') {
      if (idStr.length() == 0) {
        lcdMensaje("Ingresa un ID", "antes de continuar");
        delay(1500);
        lcd.clear();
        lcdMensaje("Ingresa ID:...", "Pulsa * para OK"); 
        ultimoScroll = millis();
        mostrandoScroll = true;
        continue;
      } else {
        break;
      }
    }

    // Números
    if (key >= '0' && key <= '9' && idStr.length() < 3) {
      idStr += key;
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Digitando ID...");
      lcd.setCursor(0, 1);
      lcd.print("ID: " + idStr + "   ");
      mostrandoScroll = false;
    }

    // Scroll
    if (idStr.length() == 0 && millis() - ultimoScroll >= intervaloScroll) {
      lcd.clear();
      lcdMensaje("Ingresa ID:...", "Pulsa * para OK"); 
      ultimoScroll = millis();
      mostrandoScroll = true;
    }
  }

  return idStr.toInt();
}

// --- Registrar huella con opción de cancelar ---
void registrarHuellaSP32() {
    cargarCache(); // cargar cache actual al iniciar

    // 1️⃣ Ingresar cédula
    String cedula = ingresarCedula();
    if (cedula == "") {
        lcdMensaje("Registro cancelado");
        delay(1000);
        return;
    }

    // 2️⃣ Verificar si la cédula existe en Firestore
    String collection = "estudiantes";
    String documentPath = cedula;
    String pathCompleto = collection + "/" + documentPath;

    if (!Firebase.Firestore.getDocument(&fbdo, PROJECT_ID, "", pathCompleto.c_str())) {
        lcdMensaje("Error al consultar", "Firestore");
        Serial.println("Error Firestore: " + fbdo.errorReason());
        delay(2000);
        return;
    }

    if (fbdo.payload().length() == 0) {
        lcdMensaje("Cédula no existe", "Firestore");
        delay(2000);
        return;
    }

    // 3️⃣ Verificar cache
    int idExistente = buscarCache(cedula);
    int idAsignado = idExistente;

    if (idExistente != -1) {
        lcdMensaje("Cédula ya registrada", "1:Actualizar 2:Cancelar");
        while (true) {
            char key = keypad.getKey();
            if (key == '1') { idAsignado = idExistente; break; }
            if (key == '2' || key == '#') { lcdMensaje("Actualización cancelada"); delay(1500); return; }
        }
    } else {
        idAsignado = obtenerIdLibre();
        if (idAsignado == -1) {
            lcdMensaje("No hay IDs libres");
            delay(2000);
            return;
        }
    }

    // 4️⃣ Registrar huella (solo 2 lecturas para crear modelo)
    int p = -1;
    for (int intento = 1; intento <= 2; intento++) {
        lcdMensaje("Coloca dedo", String(intento) + "/2");
        delay(1000);

        while ((p = finger.getImage()) != FINGERPRINT_OK) {
            char key = keypad.getKey();
            if (key == '#') { lcdMensaje("Registro cancelado"); delay(1000); return; }
            if (p == FINGERPRINT_NOFINGER) delay(100);
        }

        p = finger.image2Tz(intento);
        if (p != FINGERPRINT_OK) {
            lcdMensaje("Error, intenta otra vez", String(intento) + "/2");
            intento--; // repetir intento
            continue;
        }

        lcdMensaje("Retira el dedo", String(intento) + "/2");
        delay(1500);
        while (finger.getImage() != FINGERPRINT_NOFINGER) {
            char key = keypad.getKey();
            if (key == '#') { lcdMensaje("Registro cancelado"); delay(1000); return; }
            delay(100);
        }
    }

    // 5️⃣ Crear modelo con las 2 lecturas
    p = finger.createModel();
    if (p != FINGERPRINT_OK) { lcdMensaje("Error creando modelo"); return; }

    // 6️⃣ Guardar modelo
    p = finger.storeModel(idAsignado);
    if (p != FINGERPRINT_OK) { lcdMensaje("Error guardando huella"); return; }

    lcdMensaje("Huella registrada", "correctamente");
    delay(2000);

    // 7️⃣ Actualizar cache
    if (idExistente == -1) {
        cache[totalCache].cedula = cedula;
        cache[totalCache].idHuella = idAsignado;
        totalCache++;
    }
    guardarCache(cedula, idAsignado);

    // 8️⃣ Guardar en Firestore
    FirebaseJson content;
    content.set("fields/id_registro/integerValue", idAsignado);

    if (!Firebase.Firestore.patchDocument(&fbdo, PROJECT_ID, "", pathCompleto.c_str(), content.raw(), "id_registro")) {
        lcdMensaje("Error guardando registro");
        Serial.println("Error Firestore: " + fbdo.errorReason());
        return;
    }

    Serial.println("Registro guardado en Firestore correctamente.");
}







bool verificarHuella() {
  int intentosInternos = 3;
  int p;

  for (int i = 0; i < intentosInternos; i++) {
    // Captura la imagen del dedo
    p = finger.getImage();
    if (p != FINGERPRINT_OK) continue;

    // Convierte la imagen a template
    p = finger.image2Tz();
    if (p != FINGERPRINT_OK) continue;

    // Busca la huella en la base de datos
    p = finger.fingerSearch();
    if (p == FINGERPRINT_OK && finger.confidence >= 30) { // ajusta este valor
      lcdMensaje("Huella reconocida", "ID: " + String(finger.fingerID));
      delay(2000);
      marcarAsistencia(finger.fingerID);
      return true;  // éxito
    }
  }

  // Si no tuvo éxito en los 3 intentos
        lcdMensaje("Huella no", "reconocida");
  delay(1000);
  return false;
}
/*
void alternarModo(String botonPresionado) {
  if (botonPresionado == "registro") {
    modo = "registro";
    lcdMensaje("Modo cambiado a", "REGISTRO");
    // No se pregunta tipo en modo registro
  } else if (botonPresionado == "verificacion") {
    modo = "verificacion";
    lcdMensaje("Modo cambiado a", "VERIFICACION");
    seleccionarTipoAsistencia();  // solo aquí preguntar tipo
  }
}
*/
void imprimirCache() {
  Serial.println("===== CACHE ACTUAL =====");
  for (int i = 0; i < totalCache; i++) {
    Serial.print("Indice "); Serial.print(i);
    Serial.print(" | Cedula: "); Serial.print(cache[i].cedula);
    Serial.print(" | ID Huella: "); Serial.println(cache[i].idHuella);
  }
  Serial.println("=======================");
}

// -------------------- FUNCIÓN actualizar: ya incluida arriba --------------------
// (actualizarHorarios() está definido anteriormente y se encarga de llenar cacheHorarios[])

// -------------------- MARCAR ASISTENCIA (MODIFICADA para usar solo cacheHorarios[]) --------------------
void marcarAsistencia(int id) {
  // 1️⃣ Buscamos la cédula correspondiente en el cache usando el id de huella
  String cedula = "";
  for (int i = 0; i < totalCache; i++) {
    if (cache[i].idHuella == id) {
      cedula = cache[i].cedula;
      break;
    }
  }

  if (cedula == "") {
    lcdMensaje("Huella no en cache");
    delay(2000);
    return;
  }

  // 2️⃣ Obtener nombre desde Firestore (solo campo nombre)
  String docPath = "estudiantes/" + cedula;
  String nombre = "";
  if (Firebase.Firestore.getDocument(&fbdo, PROJECT_ID, "", docPath.c_str())) {
    FirebaseJson payload;
    payload.setJsonData(fbdo.payload());

    FirebaseJsonData data;
    payload.get(data, "fields/nombre/stringValue");
    nombre = data.stringValue;
  } else {
    lcdMensaje("Error al obtener nombre");
    delay(2000);
    return;
  }

  // 3️⃣ Obtener hora y fecha actuales
  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);

  char fechaFormatoID[11];
  strftime(fechaFormatoID, sizeof(fechaFormatoID), "%Y-%m-%d", timeinfo);  // para ID

  char fechaParaMostrar[15];
  sprintf(fechaParaMostrar, "%d/%d/%d", timeinfo->tm_mday, timeinfo->tm_mon + 1, timeinfo->tm_year + 1900);

  // Hora actual en minutos desde medianoche
  int minutosActual = timeinfo->tm_hour * 60 + timeinfo->tm_min;

  // ==========================
  // BUSQUEDA DE HORARIO USANDO LA CACHE (cacheHorarios[])
  // ==========================
  bool horarioEncontrado = false;
  String asignatura = "", profesor = "", ciclo = "";
  String horaInicioHorario = "";
  int mejorDiferencia = 999999;

  for (int i = 0; i < totalHorarios; i++) {
    Horario &h = cacheHorarios[i];
    if (h.horaIni.length() < 4 || h.horaFin.length() < 4) continue;

    int horaIniNum = 0, minIniNum = 0, horaFinNum = 0, minFinNum = 0;
    sscanf(h.horaIni.c_str(), "%d:%d", &horaIniNum, &minIniNum);
    sscanf(h.horaFin.c_str(), "%d:%d", &horaFinNum, &minFinNum);

    int minutosHoraIni = horaIniNum * 60 + minIniNum;
    int minutosHoraFin = horaFinNum * 60 + minFinNum;

    if (minutosHoraFin <= minutosHoraIni) continue;

    if (minutosActual >= minutosHoraIni && minutosActual < minutosHoraFin) {
      int diferencia = minutosActual - minutosHoraIni;
      if (diferencia >= 0 && diferencia < mejorDiferencia) {
        mejorDiferencia = diferencia;
        horaInicioHorario = h.horaIni;
        asignatura = h.asignatura;
        profesor = h.profesor;
        ciclo = h.ciclo;
        horarioEncontrado = true;
      }
    }
  }

  if (!horarioEncontrado) {
    lcdMensaje("Fuera de horario");
    delay(2000);
    grupoActual = "";
    return;
  }

  // Si es práctica, solicitar grupo
  if (tipoAsistencia == "practica") {
    int grupoIngresado = leerNumeroDesdeTeclado("Ingresa grupo: ", 2);
    grupoActual = String(grupoIngresado);
  } else {
    grupoActual = "";
  }

  // 4️⃣ Construir ID del documento de asistencia
  String idDocumento = cedula + "_" + String(fechaFormatoID) + "_" + horaInicioHorario + "_" + asignatura;
  idDocumento.replace(" ", "_");

  // Revisar si ya existe asistencia
  String pathAsistencia = "asistencias/" + idDocumento;
  if (Firebase.Firestore.getDocument(&fbdo, PROJECT_ID, "", pathAsistencia.c_str())) {
    lcdMensaje("Asistencia ya registrada");
    delay(2000);
    return;
  }

  // Crear JSON y subir asistencia
  FirebaseJson jsonAsistencia;
  jsonAsistencia.set("fields/nombre/stringValue", nombre);
  jsonAsistencia.set("fields/cedula/stringValue", cedula);
  jsonAsistencia.set("fields/fecha/stringValue", String(fechaParaMostrar));
  jsonAsistencia.set("fields/hora/stringValue", horaInicioHorario);
  jsonAsistencia.set("fields/asignatura/stringValue", asignatura);
  jsonAsistencia.set("fields/profesor/stringValue", profesor);
  jsonAsistencia.set("fields/ciclo/stringValue", ciclo);
  jsonAsistencia.set("fields/tipo/stringValue", tipoAsistencia);
  if (tipoAsistencia == "practica") {
    jsonAsistencia.set("fields/preparatorio/stringValue", preparatorioGlobal);
    jsonAsistencia.set("fields/grupo/stringValue", grupoActual);
  }

  struct tm* utcInfo = gmtime(&now);
  char timestampISO[30];
  strftime(timestampISO, sizeof(timestampISO), "%Y-%m-%dT%H:%M:%S", utcInfo);
  strcat(timestampISO, "Z");
  jsonAsistencia.set("fields/timestamp/timestampValue", String(timestampISO));

  if (Firebase.Firestore.createDocument(&fbdo, PROJECT_ID, "", "asistencias/" + idDocumento, jsonAsistencia.raw())) {
    lcd.setCursor(0,0);
    lcd.print("Ced: " + cedula);
    lcd.setCursor(0,1);
    lcd.print(abreviarAsignatura(asignatura));
    delay(2000);
  } else {
    lcdMensaje("Error, intenta otra vez");
  }
}

// ------------------------------------------------------------------------------------

int leerNumeroDesdeTeclado(String mensaje, int maxDigitos) {
  String input = "";
  lcdMensaje(mensaje, "Usa * para finalizar");

  while (true) {
    char key = keypad.getKey();
    if (key != NO_KEY) {

      // CANCELAR
      if (key == '#') {
        lcdMensaje("Operacion cancelada");
        delay(1500);
        return -1;
      }

      if (key == '*') {
        lcd.clear();
        break;
      } else if (key >= '0' && key <= '9') {
        if (input.length() < maxDigitos) {
          input += key;
          lcdMensaje("Digitando:", String(input));
        }
      } else {
        lcdMensaje("\n Tecla no valida");
      }
    }
  }

  return input.toInt();
}


String diaSemanaStr(int dia) {
  switch (dia) {
    case 0: return "domingo";
    case 1: return "lunes";
    case 2: return "martes";
    case 3: return "miercoles";
    case 4: return "jueves";
    case 5: return "viernes";
    case 6: return "sabado";
    default: return "";
  }
}
