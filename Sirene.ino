/*
 * Sistema de Sirene Automática - NodeMCU ESP8266
 * - Sirene em horários programados (NTP)
 * - Configuração por interface web (horário, dias da semana, duração em segundos)
 * - Display I2C: data/hora atual e próximo alarme
 * - Pino da sirene: D1 (GPIO5) -> HIGH para acionar
 *
 * Bibliotecas (Gerenciar Bibliotecas): WiFiManager, NTPClient, Time (TimeLib), LiquidCrystal I2C
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <TimeLib.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFiManager.h>
#include <EEPROM.h>

// --- Configuração de hardware ---
#define PINO_SIRENE      14   // D5 (GPI14) - nível HIGH aciona a sirene
#define ENDERECO_LCD     0x3F
#define COLS_LCD         17
#define LINHAS_LCD       2
#define MAX_ALARMES      10
#define EEPROM_SIZE      512
#define OFFSET_ALARMES   0

// Timezone (Brasil: -3)
#define UTC_OFFSET       -3
#define NTP_INTERVAL_MS  60000  // Atualizar NTP a cada 60 s

// Estrutura de um alarme (6 bytes)
struct Alarme {
  uint8_t  ativo;      // 0 ou 1
  uint8_t  hora;       // 0-23
  uint8_t  minuto;     // 0-59
  uint16_t duracao_s;  // duração em segundos
  uint8_t  dias;       // bit0=Dom, bit1=Seg, ... bit6=Sab
};

WiFiUDP udp;
NTPClient ntp(udp, "pool.ntp.org", UTC_OFFSET * 3600, NTP_INTERVAL_MS);
ESP8266WebServer server(80);
LiquidCrystal_I2C lcd(ENDERECO_LCD, COLS_LCD, LINHAS_LCD);

Alarme alarmes[MAX_ALARMES];
uint8_t num_alarmes = 0;
unsigned long ultima_sinc_ntp = 0;
bool sirene_ligada = false;
unsigned long sirene_ate_ms = 0;
unsigned long ultima_atualizacao_display = 0;
int ultimo_minuto_disparo = -1;  // evita disparar de novo no mesmo minuto

// TimeLib: weekday() 1=Dom, 2=Seg, ... 7=Sab
static uint8_t dia_atual_bits() {
  int w = weekday();
  if (w < 1 || w > 7) return 0;
  return (1 << (w - 1));  // bit0=Dom, bit1=Seg, ...
}

static void eeprom_ler_alarmes() {
  EEPROM.begin(EEPROM_SIZE);
  num_alarmes = EEPROM.read(OFFSET_ALARMES);
  if (num_alarmes > MAX_ALARMES) num_alarmes = 0;
  for (uint8_t i = 0; i < num_alarmes; i++) {
    uint16_t addr = OFFSET_ALARMES + 1 + i * sizeof(Alarme);
    Alarme* a = &alarmes[i];
    a->ativo    = EEPROM.read(addr + 0);
    a->hora     = EEPROM.read(addr + 1);
    a->minuto   = EEPROM.read(addr + 2);
    a->duracao_s = (EEPROM.read(addr + 3) << 8) | EEPROM.read(addr + 4);
    a->dias    = EEPROM.read(addr + 5);
  }
}

static void eeprom_gravar_alarmes() {
  EEPROM.write(OFFSET_ALARMES, num_alarmes);
  for (uint8_t i = 0; i < num_alarmes; i++) {
    uint16_t addr = OFFSET_ALARMES + 1 + i * sizeof(Alarme);
    Alarme* a = &alarmes[i];
    EEPROM.write(addr + 0, a->ativo);
    EEPROM.write(addr + 1, a->hora);
    EEPROM.write(addr + 2, a->minuto);
    EEPROM.write(addr + 3, (a->duracao_s >> 8) & 0xFF);
    EEPROM.write(addr + 4, a->duracao_s & 0xFF);
    EEPROM.write(addr + 5, a->dias);
  }
  EEPROM.commit();
}

// Encontra o próximo alarme (hoje ou futuro) e preenche hora/min e texto do dia
static bool proximo_alarme(uint8_t* out_hora, uint8_t* out_minuto, char* linha2, size_t len_linha2) {
  int agora_min = hour() * 60 + minute();
  int melhor_min = 24 * 60;
  int melhor_h = 0, melhor_m = 0;
  const char* dias_nome[] = { "Dom","Seg","Ter","Qua","Qui","Sex","Sab" };
  uint8_t dias_hoje = dia_atual_bits();

  for (uint8_t i = 0; i < num_alarmes; i++) {
    Alarme* a = &alarmes[i];
    if (!a->ativo) continue;
    if (!(a->dias & dias_hoje)) continue;
    int alarme_min = a->hora * 60 + a->minuto;
    if (alarme_min > agora_min && alarme_min < melhor_min) {
      melhor_min = alarme_min;
      melhor_h = a->hora;
      melhor_m = a->minuto;
    }
  }
  if (melhor_min < 24 * 60) {
    if (out_hora)  *out_hora  = melhor_h;
    if (out_minuto) *out_minuto = melhor_m;
    if (linha2 && len_linha2 >= 12)
      snprintf(linha2, len_linha2, "Prox: %02d:%02d     ", melhor_h, melhor_m);
    return true;
  }
  if (linha2 && len_linha2 >= 12)
    snprintf(linha2, len_linha2, "Prox: --:--      ");
  return false;
}

static void atualizar_display() {
  char linha1[COLS_LCD + 1];
  char linha2[COLS_LCD + 1];
  snprintf(linha1, sizeof(linha1), "%02d/%02d %02d:%02d:%02d  ",
           day(), month(), hour(), minute(), second());
  proximo_alarme(NULL, NULL, linha2, sizeof(linha2));
  lcd.setCursor(0, 0);
  lcd.print(linha1);
  lcd.setCursor(0, 1);
  lcd.print(linha2);
}

static void raiz_web() {
  String html = R"raw(
<!DOCTYPE html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Sirene - Configuração</title>
<style>
body{font-family:sans-serif;max-width:480px;margin:20px auto;padding:10px;background:#1a1a2e;color:#eee;}
h1{color:#0f4;}
a{color:#0cf;}
form{background:#16213e;padding:15px;border-radius:8px;margin:10px 0;}
label{display:block;margin:8px 0 2px;}
input[type="number"],input[type="time"]{width:100%;padding:8px;box-sizing:border-box;border:1px solid #0f4;}
.dias{display:flex;flex-wrap:wrap;gap:8px;margin:8px 0;}
.dias label{display:inline;margin-right:8px;}
button{background:#0f4;color:#000;border:none;padding:10px 20px;border-radius:6px;cursor:pointer;font-weight:bold;}
button.del{background:#c33;color:#fff;}
.alarme{background:#0d1b2a;padding:10px;margin:8px 0;border-radius:6px;}
</style></head><body>
<h1>Sirene Automática</h1>
<p>Data/hora do equipamento: )raw";
  html += String(day()) + "/" + String(month()) + "/" + String(year());
  html += " " + String(hour()) + ":" + String(minute()) + ":" + String(second());
  html += R"raw(</p>
<form method="POST" action="/add">
<label>Horário</label>
<input type="time" name="hora" required>
<label>Duração (segundos)</label>
<input type="number" name="duracao" value="3" min="1" max="3600">
<label>Dias da semana</label>
<div class="dias">
<label><input type="checkbox" name="d0" value="1"> Dom</label>
<label><input type="checkbox" name="d1" value="1"> Seg</label>
<label><input type="checkbox" name="d2" value="1"> Ter</label>
<label><input type="checkbox" name="d3" value="1"> Qua</label>
<label><input type="checkbox" name="d4" value="1"> Qui</label>
<label><input type="checkbox" name="d5" value="1"> Sex</label>
<label><input type="checkbox" name="d6" value="1"> Sab</label>
</div>
<button type="submit">Adicionar alarme</button>
</form>
<h2>Alarmes</h2>
)raw";
  for (uint8_t i = 0; i < num_alarmes; i++) {
    Alarme* a = &alarmes[i];
    if (!a->ativo) continue;
    const char* dias_n[] = {"Dom","Seg","Ter","Qua","Qui","Sex","Sab"};
    String dias_str;
    for (int d = 0; d < 7; d++)
      if (a->dias & (1 << d)) dias_str += String(dias_n[d]) + " ";
    html += "<div class=\"alarme\">";
    html += "<strong>" + String(a->hora < 10 ? "0" : "") + String(a->hora) + ":" + (a->minuto < 10 ? "0" : "") + String(a->minuto) + "</strong> ";
    html += String(a->duracao_s) + "s - " + dias_str;
    html += " <a href=\"/del?i=" + String(i) + "\">[Excluir]</a></div>";
  }
  html += "</body></html>";
  server.send(200, "text/html; charset=utf-8", html);
}

static void add_alarme() {
  if (server.method() != HTTP_POST) { server.send(405, "text/plain", "Method Not Allowed"); return; }
  if (num_alarmes >= MAX_ALARMES) {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
    return;
  }
  String hora_str = server.arg("hora");
  int duracao = server.arg("duracao").toInt();
  if (duracao < 1) duracao = 30;
  if (duracao > 3600) duracao = 3600;
  int h = 0, m = 0;
  if (hora_str.length() >= 5) {
    h = hora_str.substring(0, 2).toInt();
    m = hora_str.substring(3, 5).toInt();
  }
  uint8_t dias = 0;
  for (int i = 0; i < 7; i++)
    if (server.hasArg("d" + String(i))) dias |= (1 << i);
  if (dias == 0) dias = 0x7F;  // se nenhum marcado, todos
  Alarme* a = &alarmes[num_alarmes];
  a->ativo = 1;
  a->hora = (uint8_t)(h % 24);
  a->minuto = (uint8_t)(m % 60);
  a->duracao_s = (uint16_t)duracao;
  a->dias = dias;
  num_alarmes++;
  eeprom_gravar_alarmes();
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

static void del_alarme() {
  if (!server.hasArg("i")) { server.sendHeader("Location", "/"); server.send(302, "text/plain", ""); return; }
  int idx = server.arg("i").toInt();
  if (idx >= 0 && idx < (int)num_alarmes) {
    for (int i = idx; i < (int)num_alarmes - 1; i++)
      alarmes[i] = alarmes[i + 1];
    num_alarmes--;
    eeprom_gravar_alarmes();
  }
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void setup() {
  Serial.begin(115200);
  pinMode(PINO_SIRENE, OUTPUT);
  digitalWrite(PINO_SIRENE, LOW);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print(" Iniciando...   ");

  WiFiManager wm;
  if (!wm.autoConnect("SireneAP", "sirene123")) {
    Serial.println("Falha WiFi");
    lcd.setCursor(0, 1);
    lcd.print(" WiFi falhou    ");
    delay(3000);
  }

  eeprom_ler_alarmes();
  ntp.begin();
  ntp.update();
  if (ntp.getEpochTime() > 0)
    setTime(ntp.getEpochTime());
  ultima_sinc_ntp = millis();

  server.on("/", raiz_web);
  server.on("/add", add_alarme);
  server.on("/del", del_alarme);
  server.begin();

  lcd.setCursor(0, 0);
  lcd.print(" IP: ");
  lcd.print(WiFi.localIP());
  lcd.setCursor(0, 1);
  lcd.print(" Config: /      ");
  delay(2000);
}

void loop() {
  server.handleClient();

  // Sincronizar NTP periodicamente
  if (millis() - ultima_sinc_ntp >= (unsigned long)NTP_INTERVAL_MS) {
    ultima_sinc_ntp = millis();
    if (ntp.update() && ntp.getEpochTime() > 0)
      setTime(ntp.getEpochTime());
  }

  // Controle da sirene
  if (sirene_ligada) {
    if ((long)(millis() - sirene_ate_ms) >= 0) {
      digitalWrite(PINO_SIRENE, LOW);
      sirene_ligada = false;
    }
  } else {
    uint8_t dias_hoje = dia_atual_bits();
    int agora_min = hour() * 60 + minute();
    if (agora_min != ultimo_minuto_disparo) {
      for (uint8_t i = 0; i < num_alarmes; i++) {
        Alarme* a = &alarmes[i];
        if (!a->ativo) continue;
        if (!(a->dias & dias_hoje)) continue;
        if (a->hora * 60 + a->minuto != agora_min) continue;
        ultimo_minuto_disparo = agora_min;
        digitalWrite(PINO_SIRENE, HIGH);
        sirene_ligada = true;
        sirene_ate_ms = millis() + (unsigned long)a->duracao_s * 1000;
        break;
      }
    }
  }

  // Atualizar display a cada segundo
  if (millis() - ultima_atualizacao_display >= 1000) {
    ultima_atualizacao_display = millis();
    atualizar_display();
  }
}
