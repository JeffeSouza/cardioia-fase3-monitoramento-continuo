/*
 * CardioIA - Fase 3: Monitoramento Continuo (wearable cardiaco simulado)
 * Autor: Jefferson de Souza Oliveira - RM568138 - Turma 2TIAOR
 *
 * Fluxo: captura -> processamento local -> fila offline -> MQTT/Serial -> dashboard.
 *
 * Toda amostra passa pela fila circular, mesmo com conexao ativa. Ela so sai da fila
 * depois que o envio e confirmado. Com isso, online e offline usam o mesmo caminho e
 * nao existe um trecho de codigo exclusivo para a sincronizacao.
 *
 * Hardware (ver diagram.json):
 *   DHT22         GPIO15  temperatura e umidade (sensor 1)
 *   Botao         GPIO4   cada toque simula um batimento (sensor 2)
 *   Chave         GPIO18  simula o Wi-Fi: esquerda = conectado, direita = sem conexao
 *   LED verde     GPIO19  conectado
 *   LED vermelho  GPIO21  alerta local (funciona mesmo sem conexao)
 *
 * Dados simulados. Este prototipo nao e um dispositivo medico.
 */
#include <Arduino.h>
#include <DHTesp.h>
#include <MQTT.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include "ring_buffer.h"

// 1 = envia ao broker MQTT (Parte 2) e registra no Serial.
// 0 = somente Serial: o Monitor Serial faz o papel da nuvem (Parte 1).
#define MQTT_ENABLED 1

// ---------------------------------- Pinos -----------------------------------
const uint8_t PIN_DHT = 15;
const uint8_t PIN_BEAT_BUTTON = 4;
const uint8_t PIN_WIFI_SWITCH = 18;
const uint8_t PIN_LED_ONLINE = 19;
const uint8_t PIN_LED_ALERT = 21;

// ------------------------------- Configuracao --------------------------------
const char *DEVICE_ID = "cardioia-7f3a9c";  // sufixo evita colisao de topicos no broker publico
const char *WIFI_SSID = "Wokwi-GUEST";  // rede aberta do simulador

// Broker publico da HiveMQ, sem autenticacao: qualquer pessoa pode ler e publicar nos
// topicos. Aceitavel aqui porque os dados sao simulados; em producao exigiria broker
// privado e autenticacao por dispositivo (ver docs/).
const char *MQTT_HOST = "broker.hivemq.com";
const uint16_t MQTT_PORT = 8883;  // MQTT sobre TLS

const uint32_t SAMPLE_PERIOD_MS = 5000;  // uma amostra a cada 5 s

// Capacidade: 5.000 amostras x 5 s = cerca de 6,9 h sem conexao, usando 80 KB de RAM
// (16 bytes por amostra). Cobre uma saida prolongada do paciente sem rede.
#ifndef BUFFER_CAPACITY
#define BUFFER_CAPACITY 5000
#endif

const uint8_t FLUSH_PER_LOOP = 10;      // envia em lotes para nao travar a captura
const uint32_t BEAT_DEBOUNCE_MS = 80;   // filtra o repique do botao
const uint32_t BEAT_TIMEOUT_MS = 10000; // sem toques por 10 s = sem sinal (bpm 0)
const uint32_t MQTT_RETRY_MS = 5000;    // intervalo entre tentativas de conexao
const uint32_t BACKLOG_AGE_MS = 2 * SAMPLE_PERIOD_MS;  // amostra mais velha e backlog

// Limites de alerta definidos para o projeto.
const uint16_t BPM_MAX = 120;    // acima: taquicardia
const uint16_t BPM_MIN = 50;     // abaixo (com sinal): bradicardia
const int16_t TEMP_MAX_DECI = 380;  // acima de 38,0 C: febre

// ---------------------------------- Modelo -----------------------------------
struct Sample {
  uint32_t seq;         // numero de sequencia, permite detectar perdas na nuvem
  uint32_t capturedMs;  // millis() da captura; o horario real e calculado no envio
  int16_t tempDeci;     // temperatura em decimos de grau (365 = 36,5 C)
  int16_t humDeci;      // umidade em decimos de % (550 = 55,0 %)
  uint16_t bpm;         // 0 = sem sinal
};
static_assert(sizeof(Sample) == 16, "Sample deve ocupar 16 bytes");

const int16_t SENSOR_FAIL = INT16_MIN;  // marca uma leitura invalida do DHT22

enum AlertFlags : uint8_t { ALERT_TACHY = 1, ALERT_BRADY = 2, ALERT_FEVER = 4 };

RingBuffer<Sample> buffer;
DHTesp dht;
uint32_t nextSeq = 1;
uint32_t lastSampleMs = 0;
bool linkUp = false;  // ultimo estado de conexao informado no Serial

// --------------------------------- Batimentos ---------------------------------
// A interrupcao registra o instante de cada toque, sem perder batimentos enquanto o
// loop esta ocupado com a rede.
const uint8_t BEAT_WINDOW = 6;  // 6 toques = 5 intervalos na media
volatile uint32_t beatTimes[BEAT_WINDOW];
volatile uint8_t beatCount = 0;
portMUX_TYPE beatMux = portMUX_INITIALIZER_UNLOCKED;

void IRAM_ATTR onBeat() {
  const uint32_t now = millis();
  portENTER_CRITICAL_ISR(&beatMux);
  const uint32_t last = beatCount ? beatTimes[beatCount - 1] : 0;
  if (beatCount == 0 || now - last >= BEAT_DEBOUNCE_MS) {
    if (beatCount && now - last > BEAT_TIMEOUT_MS) beatCount = 0;  // nova sequencia
    if (beatCount == BEAT_WINDOW) {
      for (uint8_t i = 1; i < BEAT_WINDOW; i++) beatTimes[i - 1] = beatTimes[i];
      beatCount = beatCount - 1;
    }
    beatTimes[beatCount] = now;
    beatCount = beatCount + 1;
  }
  portEXIT_CRITICAL_ISR(&beatMux);
}

// Calcula o BPM pela media dos ultimos intervalos. Contar toques por 60 s deixaria a
// leitura lenta demais para reagir a uma mudanca de ritmo.
uint16_t currentBpm() {
  portENTER_CRITICAL(&beatMux);
  const uint8_t n = beatCount;
  const uint32_t first = n ? beatTimes[0] : 0;
  const uint32_t last = n ? beatTimes[n - 1] : 0;
  portEXIT_CRITICAL(&beatMux);

  // Exige ao menos 3 toques: com 2, um intervalo isolado geraria um falso alerta.
  if (n < 3 || millis() - last > BEAT_TIMEOUT_MS) return 0;
  const uint32_t avgInterval = (last - first) / (n - 1);
  return avgInterval ? (uint16_t)(60000UL / avgInterval) : 0;
}

// ----------------------------------- Alertas -----------------------------------
uint8_t evaluateAlerts(const Sample &s) {
  uint8_t flags = 0;
  if (s.bpm > BPM_MAX) flags |= ALERT_TACHY;
  if (s.bpm > 0 && s.bpm < BPM_MIN) flags |= ALERT_BRADY;
  if (s.tempDeci != SENSOR_FAIL && s.tempDeci > TEMP_MAX_DECI) flags |= ALERT_FEVER;
  return flags;
}

const char *alertText(uint8_t flags) {
  switch (flags) {
    case 0: return "normal";
    case ALERT_TACHY: return "taquicardia";
    case ALERT_BRADY: return "bradicardia";
    case ALERT_FEVER: return "febre";
    case ALERT_TACHY | ALERT_FEVER: return "taquicardia+febre";
    case ALERT_BRADY | ALERT_FEVER: return "bradicardia+febre";
    default: return "indefinido";
  }
}

// ------------------------------------ JSON -------------------------------------
// Horario real da captura = agora - idade da amostra. Assim, amostras coletadas antes
// de o relogio sincronizar tambem recebem o horario correto. Retorna 0 se nao houver NTP.
uint32_t captureEpoch(const Sample &s) {
  const time_t now = time(nullptr);
  if (now < 1700000000) return 0;
  return (uint32_t)now - (millis() - s.capturedMs) / 1000;
}

void formatJson(const Sample &s, bool backlog, char *out, size_t len) {
  char temp[8] = "null", hum[8] = "null";  // sensor com falha vira null no JSON
  if (s.tempDeci != SENSOR_FAIL) snprintf(temp, sizeof temp, "%.1f", s.tempDeci / 10.0f);
  if (s.humDeci != SENSOR_FAIL) snprintf(hum, sizeof hum, "%.1f", s.humDeci / 10.0f);
  snprintf(out, len,
           "{\"device\":\"%s\",\"seq\":%lu,\"ts\":%lu,\"temp\":%s,\"hum\":%s,\"bpm\":%u,"
           "\"alert\":\"%s\",\"backlog\":%s,\"queue\":%u,\"dropped\":%lu}",
           DEVICE_ID, (unsigned long)s.seq, (unsigned long)captureEpoch(s), temp, hum, s.bpm,
           alertText(evaluateAlerts(s)), backlog ? "true" : "false", (unsigned)buffer.size() - 1,  // fila sem esta amostra
           (unsigned long)buffer.dropped());
}

// ------------------------------------ Rede -------------------------------------
// Conectividade simulada: a variavel booleana pedida no enunciado, controlada pela chave.
bool connectivityEnabled() { return digitalRead(PIN_WIFI_SWITCH) == LOW; }

#if MQTT_ENABLED
WiFiClientSecure tlsClient;
MQTTClient mqtt(512);
String topicVitals, topicBacklog, topicStatus, clientId;
uint32_t lastMqttAttemptMs = 0;
bool ntpStarted = false;

void setupNetwork() {
  topicVitals = String("cardioia/") + DEVICE_ID + "/vitals";
  topicBacklog = String("cardioia/") + DEVICE_ID + "/backlog";
  topicStatus = String("cardioia/") + DEVICE_ID + "/status";
  // No broker publico, dois clientes com o mesmo ID derrubam um ao outro.
  clientId = String(DEVICE_ID) + "-" + String((uint32_t)esp_random(), HEX);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, "", 6);  // canal 6 acelera a conexao no Wokwi

  // TLS criptografa o trafego, mas setInsecure() nao valida o certificado do broker.
  // Limitacao aceita no prototipo e documentada no relatorio.
  tlsClient.setInsecure();
  mqtt.begin(MQTT_HOST, MQTT_PORT, tlsClient);
  mqtt.setOptions(30, true, 3000);  // keep-alive 30 s, sessao limpa, timeout 3 s
  // Last Will: se o dispositivo cair sem avisar, o broker publica "offline".
  mqtt.setWill(topicStatus.c_str(), "offline", true, 1);
}

// Retorna true quando o MQTT esta pronto. Nunca fica bloqueada a espera do Wi-Fi: a
// captura precisa continuar enquanto a rede nao volta.
bool serviceNetwork() {
  if (!connectivityEnabled()) {
    if (mqtt.connected()) {
      mqtt.publish(topicStatus.c_str(), "offline", true, 1);
      mqtt.disconnect();
    }
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) return false;
  if (!ntpStarted) {
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    ntpStarted = true;
  }
  if (!mqtt.connected()) {
    if (lastMqttAttemptMs && millis() - lastMqttAttemptMs < MQTT_RETRY_MS) return false;
    lastMqttAttemptMs = millis();
    Serial.println(F("[REDE] conectando ao broker..."));
    if (!mqtt.connect(clientId.c_str())) {
      Serial.printf("[REDE] falha no MQTT, erro %d\n", mqtt.lastError());
      return false;
    }
    mqtt.publish(topicStatus.c_str(), "online", true, 1);
  }
  mqtt.loop();
  return true;
}

// QoS 1: publish() so retorna true depois do PUBACK do broker.
bool transmit(const char *json, bool backlog) {
  const String &topic = backlog ? topicBacklog : topicVitals;
  if (!mqtt.publish(topic.c_str(), json, false, 1)) return false;
  Serial.printf("[CLOUD] %s %s\n", topic.c_str(), json);
  return true;
}
#else
// Modo somente Serial: o Monitor Serial faz o papel da nuvem, como permite o enunciado.
void setupNetwork() {}
bool serviceNetwork() { return connectivityEnabled(); }
bool transmit(const char *json, bool) {
  Serial.print(F("[CLOUD] "));
  Serial.println(json);
  return true;
}
#endif

// ------------------------------- Captura e envio --------------------------------
void captureSample() {
  const TempAndHumidity reading = dht.getTempAndHumidity();
  Sample s;
  s.seq = nextSeq++;
  s.capturedMs = millis();
  s.tempDeci = isnan(reading.temperature) ? SENSOR_FAIL : (int16_t)lround(reading.temperature * 10);
  s.humDeci = isnan(reading.humidity) ? SENSOR_FAIL : (int16_t)lround(reading.humidity * 10);
  s.bpm = currentBpm();

  const bool keptAll = buffer.push(s);
  digitalWrite(PIN_LED_ALERT, evaluateAlerts(s) ? HIGH : LOW);  // alerta local imediato

  Serial.printf("[EDGE] seq=%lu temp=%.1f hum=%.1f bpm=%u alerta=%s fila=%u/%u\n",
                (unsigned long)s.seq, s.tempDeci / 10.0f, s.humDeci / 10.0f, s.bpm,
                alertText(evaluateAlerts(s)), (unsigned)buffer.size(),
                (unsigned)buffer.capacity());
  if (!keptAll) {
    Serial.printf("[EDGE] fila cheia: amostra mais antiga descartada (total %lu)\n",
                  (unsigned long)buffer.dropped());
  }
}

// Envia em ordem, da mais antiga para a mais nova. Em caso de falha, a amostra continua
// na fila e o envio e retomado depois, preservando a ordem.
void flushBuffer() {
  Sample s;
  char json[256];
  for (uint8_t i = 0; i < FLUSH_PER_LOOP && buffer.peek(s); i++) {
    // Amostras antigas vao para outro topico, para nao distorcer o painel em tempo real.
    const bool backlog = millis() - s.capturedMs > BACKLOG_AGE_MS;
    formatJson(s, backlog, json, sizeof json);
    if (!transmit(json, backlog)) {
      Serial.println(F("[REDE] envio nao confirmado; amostra mantida na fila"));
      return;
    }
    buffer.pop();  // equivale a apagar o registro local apos o envio
  }
}

void reportLinkChange(bool online) {
  if (online == linkUp) return;
  linkUp = online;
  digitalWrite(PIN_LED_ONLINE, online ? HIGH : LOW);
  if (online) {
    Serial.printf("[REDE] ONLINE: sincronizando %u amostra(s)\n", (unsigned)buffer.size());
  } else {
    Serial.println(F("[REDE] OFFLINE: coleta continua na fila local"));
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BEAT_BUTTON, INPUT_PULLUP);
  pinMode(PIN_WIFI_SWITCH, INPUT_PULLUP);
  pinMode(PIN_LED_ONLINE, OUTPUT);
  pinMode(PIN_LED_ALERT, OUTPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_BEAT_BUTTON), onBeat, FALLING);
  dht.setup(PIN_DHT, DHTesp::DHT22);
  if (!buffer.begin(BUFFER_CAPACITY)) {
    Serial.println(F("[BOOT] ERRO: sem memoria para a fila; amostras nao serao retidas"));
  }
  setupNetwork();
  Serial.printf("[BOOT] CardioIA | modo=%s | fila=%u amostras | periodo=%lu ms | heap livre=%lu\n",
                MQTT_ENABLED ? "MQTT" : "somente Serial", (unsigned)buffer.capacity(),
                (unsigned long)SAMPLE_PERIOD_MS, (unsigned long)ESP.getFreeHeap());
}

void loop() {
  const uint32_t now = millis();
  if (now - lastSampleMs >= SAMPLE_PERIOD_MS) {
    lastSampleMs = now;
    captureSample();
  }
  const bool online = serviceNetwork();
  reportLinkChange(online);
  if (online) flushBuffer();
  delay(10);
}
