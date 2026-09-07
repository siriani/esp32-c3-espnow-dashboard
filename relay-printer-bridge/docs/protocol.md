# Protocolo e uso — ponte serial → paralela

## Camada física (lado do PC)

- **UART0 do ESP32 pela USB** (o mesmo canal usado para gravar o firmware).
- 8N1, sem controle de fluxo por hardware.
- Baud: `SERIAL_BAUD` (padrão **115200**). Dá para subir (ex.: 921600) editando
  o `platformio.ini`, mas a impressora é o gargalo — a LX-810L imprime na casa
  de algumas centenas de caracteres/s e tem buffer pequeno.
- **XON/XOFF** (opcional, ligado por padrão): quando a fila interna enche, o
  ESP32 manda `XOFF` (0x13) para o host; ao esvaziar, manda `XON` (0x11). Serve
  para não perder bytes ao despejar arquivos grandes.

## Fluxo de dados

Tudo que chega na Serial é enviado para a impressora **byte a byte**, com o
handshake Centronics completo (espera `BUSY` baixo → coloca D0–D7 → pulso em
`/STROBE`). Antes de cada byte o firmware também checa `SELECT` (on-line),
`PE` (papel) e `/ERROR`.

### MODO TEXTO (padrão, `-D PRN_TEXT_MODE=1`)

`CR`, `LF` e `CRLF` são normalizados para **`CRLF`**. É o que você quer para
imprimir texto digitado num terminal (senão a LX-810L faz "escada" ou
sobrescreve a linha).

### MODO RAW (`-D PRN_TEXT_MODE=0`)

O fluxo passa **intacto**. Use para enviar ESC/P binário, gráficos bit-image,
código de barras, etc. Recomendado combinar com `-D PRN_NO_BANNER` e enviar
`ESC @` no início do job.

> Observação: na energização, o **bootloader ROM do ESP32** sempre cospe uma
> linha (`rst:0x1...`) na UART0 a 115200. Isso é inevitável nessa ponte por
> UART0. Em MODO RAW, comece o job com `ESC @` para a impressora ignorar
> qualquer resquício.

---

## Como enviar (macOS / Linux)

Descubra a porta:

```bash
pio device list
# macOS:  /dev/cu.usbserial-XXXX  ou  /dev/cu.wchusbserialXXXX  ou  /dev/cu.SLAB_USBtoUART
# Linux:  /dev/ttyUSB0  ou  /dev/ttyACM0
```

### 1) Monitor serial (digitar e imprimir)

```bash
pio device monitor
# aguarde a linha "PRONTO." e digite; cada Enter imprime a linha
```

### 2) Uma linha rápida

```bash
# macOS
printf 'Ola mundo\r\n\f' > /dev/cu.usbserial-XXXX
# Linux
printf 'Ola mundo\r\n\f' > /dev/ttyUSB0
```

`\f` (0x0C, form feed) ejeta/avança a página.

Se o SO "bagunçar" a porta, fixe os parâmetros antes:

```bash
# macOS
stty -f /dev/cu.usbserial-XXXX 115200 raw -echo
# Linux (note o -F)
stty -F /dev/ttyUSB0 115200 raw -echo ixoff
```

### 3) Um arquivo inteiro

```bash
cat relatorio.txt > /dev/cu.usbserial-XXXX
```

### 4) Python (pyserial)

```python
import serial, time
p = serial.Serial("/dev/cu.usbserial-XXXX", 115200)
time.sleep(2)                      # espera o ESP32 reiniciar
p.write("ESP32 + LX-810L\r\n".encode("cp850"))
p.write(b"\x0C")                   # form feed
p.close()
```

### 5) Pela rede (HTTP)  —  `WEBPRINT_ENABLE` (padrão)

O ESP32 sobe em WiFi STA (credenciais `WEBPRINT_WIFI_SSID` / `_PASS` em
`include/config.h`, por padrão as do relay) e serve um HTTP na porta 80. O IP
aparece no monitor serial: `[web] pronto -> http://192.168.x.y/`. Também dá
para usar `http://impressora.local/` (mDNS, nome em `WEBPRINT_HOSTNAME`).

| Rota | Método | O que faz |
|------|--------|-----------|
| `/` | GET | página HTML: `<textarea>` + botão **Imprimir** + selo de estado |
| `/print` | POST | enfileira o texto: campo `texto` do formulário **ou** o corpo cru com `Content-Type: text/plain` |
| `/status` | GET | JSON: `pronta`, `estado`, `fila_livre`, `fila_total`, `fila_vazia`, `ip` |

```bash
# corpo cru (precisa do Content-Type: text/plain)
curl -sS --data-binary $'Relatorio\r\n\f' -H 'Content-Type: text/plain' \
     http://impressora.local/print
# um arquivo inteiro
curl -sS --data-binary @relatorio.txt -H 'Content-Type: text/plain' \
     http://impressora.local/print
# via campo de formulário
curl -sS --data-urlencode 'texto=Ola mundo' http://impressora.local/print
# estado
curl -sS http://impressora.local/status
```

Sem o header `Content-Type: text/plain`, o `curl --data*` envia como
`application/x-www-form-urlencoded` — aí é preciso usar o campo `texto`
(`--data-urlencode 'texto=...'`). O texto recebido cai na **mesma fila** da
ponte serial e passa pelo MODO TEXTO (CR/LF → CRLF). Limites: `WEBPRINT_MAX_BODY`
(16 KB) por requisição; `WEBPRINT_FEED_TIMEOUT_MS` (20 s) para a fila escoar.
Token opcional: `WEBPRINT_TOKEN` != `""` exige `?token=...` ou header
`X-Auth-Token` em `/print` e `/status`. Desligar tudo: `-D WEBPRINT_ENABLE=0`.

---

## ESC/P — referência rápida (LX-810L, 9 agulhas)

| Bytes            | Efeito |
|------------------|--------|
| `1B 40` (`ESC @`)| reset do interpretador |
| `0C` (`FF`)      | avança página |
| `0A` (`LF`)      | avança 1 linha |
| `0D` (`CR`)      | retorna o carro |
| `1B 45` / `1B 46`| negrito on / off |
| `1B 34` / `1B 35`| itálico on / off |
| `1B 2D 01` / `1B 2D 00` | sublinhado on / off |
| `1B 78 01` / `1B 78 00` | qualidade NLQ / rascunho |
| `1B 30` / `1B 32`| entrelinha 1/8" / 1/6" |
| `1B 43 n`        | comprimento de página = n linhas |
| `1B 43 00 n`     | comprimento de página = n polegadas |
| `0F` (`SI`) / `12` (`DC2`) | condensado on / off |
| `0E` (`SO`) / `14` (`DC4`) | expandido (dupla largura) só nesta linha |
| `1B 52 n`        | conjunto nacional de caracteres |
| `1B 74 n`        | seleciona a tabela de caracteres |
| `1B 36`          | habilita 80h–9Fh como imprimíveis |

### Acentuação / português

A LX-810L usa **tabelas de 1 byte** (PC437, **PC850**, **PC860 Portugal**, …).
Um terminal moderno manda **UTF-8** (multibyte) → o acento sai errado.

Soluções:
1. Configure a tabela na impressora (chaves DIP ou `ESC t n` / `ESC R n`) e envie
   o texto já em **CP850** ou **CP860** (como no exemplo Python acima).
2. Conversão automática UTF-8 → CP850 dentro do firmware está no **roadmap**
   (`README.md`).

---

## Solução de problemas

| Sintoma | Causa provável | O que fazer |
|---------|----------------|-------------|
| Nada imprime, **LED piscando** | off-line / sem papel / erro; `SELECT` baixo | ligar a impressora, colocar papel, apertar *On Line*; conferir divisores e **GND comum** |
| Nada imprime, **sem erro** | `/SELECT-IN` (DB25-17) não está no GND; ou chave de auto-select | aterrar o DB25-17; ou ajustar a DIP |
| Sai em "**escada**" (sem voltar o carro) | MODO RAW e o host manda só `\n` | usar MODO TEXTO, ou mandar `\r\n` |
| **Espaçamento duplo** | `/AUTOFEED` ativo + já mandamos CRLF | tirar DB25-14 do GND / ligar em **+5V** |
| **Caracteres trocados / lixo** | nível ou timing dos dados; TXS0108E instável | conferir `VCCB=5V` e GND; **trocar por 74HCT541/245**; baixar o baud |
| Só imprime **depois de muito texto** | buffer da impressora (normal) | mandar `FF` ou `\r\n` no fim do job |
| **Perde caracteres** em arquivo grande | falta controle de fluxo no host | manter `PRN_XONXOFF=1` **e** `stty ... ixoff`, ou reduzir o baud |
| ESP32 **reinicia** ao abrir o monitor / gravar | normal (pulso DTR/RTS do USB-serial) | aguardar o banner `PRONTO.` |
| Acentos errados | UTF-8 vs. tabela de 1 byte | ver seção "Acentuação" acima |
| `impressora.local` **não resolve** | mDNS bloqueado na rede, ou SO sem Bonjour/avahi | usar o IP direto (monitor serial); no Linux, instalar `avahi-daemon` |
| `POST /print` → **503** | impressora off-line / sem papel / erro | mesmo checklist do "LED piscando" |
| `POST /print` → **504** | a fila não escoou (impressora lenta ou presa) | conferir a impressora; reenviar; subir `WEBPRINT_FEED_TIMEOUT_MS` |
| Página abre mas **"estado: —"** | `/status` barrado por token, ou JS desligado | conferir `WEBPRINT_TOKEN`; imprimir pelo formulário funciona mesmo sem JS |
| Web não conecta / sem IP no log | SSID/senha errados em `WEBPRINT_WIFI_*` | corrigir em `include/config.h`; o log mostra `WiFi sem associacao` |
