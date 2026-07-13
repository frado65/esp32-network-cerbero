# Comandi utili
idf_build.sh
idf_clean_build.sh
idf_flash+monitor.ttyACM0.sh
idf_flash.ttyACM0.sh
idf_fullclean_build.sh
idf_monitor.sh

### Compilazione (build)
- `idf_build.sh`
```sh
source ~/esp/esp-idf/export.sh && idf.py build
```

- `idf_clean_build.sh`
Per forzare una compilazione - `clean` cancella i binari e i file oggetto (`.o`) precedentemente compilati, ma mantiene intatta la configurazione di CMake e le regole generate dal menuconfig. È relativamente veloce.
```sh
source ~/esp/esp-idf/export.sh && idf.py clean build
```

- `idf_fullclean_build.sh`
Per forzare una compilazione globale - `fullclean` - elimina brutalmente l'intera cartella `build`. Questo costringe CMake a ricreare l'intero albero del progetto partendo da zero, ricaricando il `Kconfig` e rigenerando tutti i collegamenti.
```sh
source ~/esp/esp-idf/export.sh && idf.py fullclean build
```

### Compila + Falsh + run
- `idf_flash.ttyACM0.sh`
```sh
source ~/esp/esp-idf/export.sh && idf.py -p /dev/ttyACM0 flash 
```

### Compila + Falsh + run + monitor
- `idf_flash+monitor.ttyACM0.sh`
```sh
source ~/esp/esp-idf/export.sh && idf.py -p /dev/ttyACM0 flash monitor
```

### Menu di configurazione di IDF
```sh
source ~/esp/esp-idf/export.sh && idf.py menuconfig
```
Nb: a volte è poi bene fare un `fullclean`.

---

# ESP32 Network Cerbero 🐕‍🦺🌐

Un watchdog hardware standalone basato su ESP32 e ESP-IDF per il monitoraggio continuo e multilivello dello stato della rete.

Invece di limitarsi a verificare la presenza del segnale Wi-Fi, "Cerbero" sonda attivamente la catena di connettività a tre livelli distinti: il router locale (LAN), l'accesso a Internet (WAN) e la risoluzione dei nomi a dominio (DNS). Fornisce un feedback visivo immediato tramite LED per diagnosticare a colpo d'occhio l'origine di un disservizio di rete.

## 🎯 Obiettivi del Progetto

1. **Diagnostica Attiva:** Andare oltre lo stato del link Wi-Fi, verificando l'effettivo instradamento dei pacchetti verso l'esterno.
2. **Isolamento del Guasto:** Capire istantaneamente se il problema è locale (router bloccato), dell'ISP (fibra/ADSL assente) o dei server DNS.
3. **Provisioning Autonomo:** Permettere la configurazione del dispositivo da qualsiasi smartphone senza dover ricompilare il firmware o utilizzare app dedicate, tramite un Captive Portal / Web Server integrato.
4. **Resilienza:** Sopravvivere a interruzioni di corrente e corruzioni della memoria flash grazie a una gestione robusta dell'NVS (Non-Volatile Storage).

## ⚙️ Funzionamento ad Alto Livello

Il ciclo di vita del firmware è gestito in due fasi principali.

### 1. Avvio e Provisioning (Modalità Access Point)
Al boot, il sistema interroga l'NVS. Se non trova credenziali Wi-Fi valide (primo avvio assoluto o reset), l'ESP32 avvia la modalità **Access Point (AP)** creando la rete `ESP32_Config`. 
Collegandosi a questa rete e navigando su `http://192.168.4.1`, viene servita una pagina web nativa che permette di configurare:
* SSID e Password della rete locale.
* Target IP per il test WAN (es. `8.8.8.8`).
* Target Host per il test DNS (es. `google.com`).

Salvando i dati, l'ESP li scrive nell'NVS e si riavvia autonomamente.

### 2. Operatività Normale (Modalità Station)
Se i parametri sono presenti in NVS, l'ESP32 si connette al router in modalità **Station (STA)**. 
Il sistema lancia un task FreeRTOS dedicato alla diagnostica che esegue un loop infinito:
1. **Check LAN:** Effettua un ping al Gateway (Router).
2. **Check WAN:** Effettua un ping all'IP esterno salvato.
3. **Check DNS:** Tenta la risoluzione tramite le API POSIX (`getaddrinfo`) e pinge l'IP risultante.

Il Web Server HTTP rimane attivo in background, accessibile tramite l'IP assegnato dal router, permettendo di modificare i bersagli dei test "al volo" senza interrompere il funzionamento.

## 💡 Scelte Progettuali

* **Nessuna dipendenza esterna per la Web UI:** Invece di usare file system complessi (SPIFFS/FATFS) per ospitare HTML/CSS, l'interfaccia risiede direttamente nella memoria programma (`.rodata`) come template stringa in C. I valori attuali vengono iniettati a runtime tramite `snprintf`. Questo garantisce massima velocità, zero frammentazione della flash e un ingombro di memoria trascurabile.
* **Separazione dei Task (FreeRTOS):** Lo stack TCP/IP, il server HTTP e il loop di diagnostica ICMP (Ping) girano su task separati. Questo evita che un timeout nella risoluzione DNS blocchi l'interfaccia web o il driver Wi-Fi.
* **Gestione NVS "Fail-Safe":** In caso di corruzione delle tabelle delle partizioni (es. sbalzi di tensione durante una scrittura), il wrapper di inizializzazione NVS formatta e recupera autonomamente il file system, prevenendo il *bricking* della scheda. Le struct in RAM vengono inizializzate rigorosamente a zero (`memset`) per garantire uno stato logico predicibile al primo boot.
* **API POSIX per il Networking:** Utilizzo di `getaddrinfo()` fornito da lwIP per la risoluzione DNS, garantendo conformità agli standard, thread-safety e scalabilità futura verso IPv6.
* **Gestione Hardware tramite LEDC/MCPWM**: Il *feedback sonoro* è implementato tramite il driver LEDC (LED Control) per generare frequenze di tono precise, gestite in modo asincrono per non impattare sui timing critici dei socket lwIP.

## 🚨 Feedback Visivo e Sonoro 🔔

Il sistema fornisce un feedback immediato su due canali: LED per lo stato visivo e Buzzer per l'avviso acustico. I pattern di segnalazione permettono di identificare rapidamente il punto di rottura della catena di rete.

| Stato Diagnostica | LED Pattern | Feedback Sonoro (Buzzer) |
| :--- | :--- | :--- |
| **Operativo** | Fisso | Silenzioso |
| **Fail LAN** | Rapido (100ms) | Beep brevi e incalzanti |
| **Fail WAN** | Medio (200ms) | Tono pulsato intermittente |
| **Fail DNS** | Lento (500ms) | Tono prolungato di avviso |

*Nota hardware: Il buzzer è collegato al GPIO 3 accoppiato in AC tramite un condensatore da 100nF, garantendo sicurezza per l'ESP32 e una risposta audio efficiente.*

## 🛠️ Requisiti e Compilazione

* **Hardware:** Qualsiasi devboard basata su ESP32 (Testato su architettura core Xtensa/RISC-V).
* **Framework:** ESP-IDF v5.x
* **Build:**
  ```bash
  idf.py set-target esp32  # O esp32c3, esp32s3, ecc.
  idf.py build flash monitor

---

# 🚧 TODO
- Aggiungere button per forzare modalità **Access Point (AP)** - FATTO.
- Fare in modo che quando si salvano i parametri venga ricaricata la pagina di settings - RISTRUTTURATO PAGINE WEB.
- 

---
---
