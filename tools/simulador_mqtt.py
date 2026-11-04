"""Simulador de dispositivo CardioIA para testar o dashboard sem o Wokwi.

Publica nos mesmos topicos e no mesmo formato JSON do firmware, com dados claramente
simulados. Ele testa o caminho broker -> Node-RED; nao substitui o teste do ESP32.

Uso (broker publico, com TLS):
    python simulador_mqtt.py --cenario completo

Uso (broker local, sem TLS):
    python simulador_mqtt.py --host localhost --port 1883 --sem-tls

Cenarios: normal, taquicardia, febre, sem_sinal, backlog, completo.
"""
import argparse
import json
import random
import secrets
import time

import paho.mqtt.client as mqtt

DEVICE = "cardioia-7f3a9c"
T_VITALS = f"cardioia/{DEVICE}/vitals"
T_BACKLOG = f"cardioia/{DEVICE}/backlog"
T_STATUS = f"cardioia/{DEVICE}/status"


def classify(bpm, temp):
    """Mesmas regras do firmware."""
    alerts = []
    if bpm > 120:
        alerts.append("taquicardia")
    if 0 < bpm < 50:
        alerts.append("bradicardia")
    if temp > 38:
        alerts.append("febre")
    return "+".join(alerts) or "normal"


class Device:
    def __init__(self, client):
        self.client = client
        self.seq = 0

    def sample(self, bpm, temp, ts, backlog=False, queue=0):
        self.seq += 1
        return {
            "device": DEVICE, "seq": self.seq, "ts": int(ts), "temp": round(temp, 1),
            "hum": round(random.uniform(50, 60), 1), "bpm": bpm,
            "alert": classify(bpm, temp), "backlog": backlog, "queue": queue, "dropped": 0,
        }

    def send(self, bpm, temp, ts=None, backlog=False, queue=0):
        payload = self.sample(bpm, temp, ts or time.time(), backlog, queue)
        topic = T_BACKLOG if backlog else T_VITALS
        self.client.publish(topic, json.dumps(payload), qos=1).wait_for_publish()
        print(topic, payload)

    def live(self, bpm_range, temp_range, n, period):
        for _ in range(n):
            self.send(random.randint(*bpm_range), random.uniform(*temp_range))
            time.sleep(period)

    def backlog(self, minutes_ago, n):
        """Envia amostras antigas em rajada, como o ESP32 ao reconectar."""
        start = time.time() - minutes_ago * 60
        for i in range(n):
            bpm = 130 if n // 2 <= i < n // 2 + 3 else random.randint(70, 90)
            self.send(bpm, random.uniform(36.4, 36.9), start + i * 5, True, n - i - 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="broker.hivemq.com")
    ap.add_argument("--port", type=int, default=8883)
    ap.add_argument("--sem-tls", action="store_true")
    ap.add_argument("--cenario", default="completo")
    ap.add_argument("--periodo", type=float, default=2.0)
    args = ap.parse_args()

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"{DEVICE}-sim-{secrets.token_hex(3)}")
    if not args.sem_tls:
        client.tls_set()
    client.will_set(T_STATUS, "offline", qos=1, retain=True)
    client.connect(args.host, args.port, keepalive=30)
    client.loop_start()
    client.publish(T_STATUS, "online", qos=1, retain=True).wait_for_publish()

    dev = Device(client)
    normal = ((70, 90), (36.4, 36.9))
    p = args.periodo
    c = args.cenario
    if c in ("normal", "completo"):
        dev.live(*normal, 6, p)
    if c in ("taquicardia", "completo"):
        dev.live((130, 150), (36.5, 36.9), 4, p)
    if c in ("febre", "completo"):
        dev.live((80, 95), (38.4, 38.9), 4, p)
    if c == "sem_sinal":
        dev.live((0, 0), (36.5, 36.8), 3, p)
    if c in ("backlog", "completo"):
        client.publish(T_STATUS, "offline", qos=1, retain=True).wait_for_publish()
        time.sleep(p)
        client.publish(T_STATUS, "online", qos=1, retain=True).wait_for_publish()
        dev.backlog(5, 20)
    if c == "completo":
        dev.live(*normal, 4, p)

    client.loop_stop()
    client.disconnect()


if __name__ == "__main__":
    main()
