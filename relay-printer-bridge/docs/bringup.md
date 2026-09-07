# Bring-up / diagnóstico (offline)

Roteiro para achar por que a impressora não aceita dados. Faça **na ordem**,
com multímetro, sem pular. Cada passo elimina uma causa.

Pinout atual do firmware: **Manawyrm/ESP32_VirtualPrinter** (ver `include/config.h`).
Firmware de teste: `pio run -e bench -t upload` (ignora status, só handshake de BUSY,
comandos `@DIAG` / `@PINS` / `@D0..@D7` / `@ST` pela serial).

---

## 0. Baseline (já confirmado neste projeto)

- [x] Impressora funciona — autoteste imprime (segura **LF/FF** e liga).
- [x] Firmware roda, mostra banner, responde comandos.
- [x] `/STROBE` (GPIO13) tem continuidade até o pino 1 do Centronics e **pulsa**.
- [x] GND do ESP32 tem continuidade até a carcaça do Centronics.
- [x] Linha de dados em nível alto ≈ 3,3 V no borne.

**Mesmo assim a impressora não levanta BUSY em resposta ao strobe.** O que falta
verificar está abaixo.

---

## 1. O `/STROBE` desce até 0 V?  (causa mais provável)

A impressora latcha o dado na **borda de descida** do `/STROBE`. Se a linha só
oscila entre 5 V e ~3,3 V (nunca abaixo de 0,8 V), a impressora **nunca vê o
strobe**.

1. Deixe **só** `GPIO13 → borne 1`. **Remova qualquer pull-up / resistor para +5 V** nessa linha.
2. Multímetro DC, borne 1 em repouso → deve dar **~3,3 V** (não 5 V).
   - Deu ~5 V? Há um fio/ponte ligando o borne 1 ao +5 V. **Ache e remova.** É o defeito.
3. Rode `@ST` em loop e meça o borne 1 → a média deve **cair** (pisca abaixo de 5 V).
   Melhor ainda: osciloscópio / analisador lógico deve mostrar pulso indo a **0 V**.

## 2. O DB25 do shield está mapeado como você acha?

Muitos breakouts numeram os terminais fora de ordem, ou a serigrafia não bate
com o pino do conector.

- Continuidade: **borne "1"** ↔ **pino físico 1 do conector DB25** da placa.
- Repita para os bornes 2..9 e 11. Se algum não bate → use o número do conector, não o do borne.

## 3. O cabo é reto DB25 ↔ Centronics-36?

- Continuidade **borne 1 do shield** ↔ **pino 1 do Centronics** (atrás da impressora), com o cabo ligado nos dois lados.
- Idem borne 2 ↔ pino 2, borne 11 ↔ pino 11, borne GND ↔ pino 19/16/33.
- Algum não apita → cabo errado, mal encaixado, ou quebrado. Cabo IEEE-1284 "A-B":
  ponta do PC = DB25 **macho**, ponta da impressora = Centronics-36 **macho**.

## 4. `/SELECT-IN` (Centronics 36 / DB25 17) está BAIXO na impressora?

Sem isso a LX-810 fica "surda" (BUSY fica baixo, sem ACKNLG, ignora o strobe —
manual técnico, Tabela 1-12).

- O firmware já dirige `GPIO19 → DB25 17` em nível **BAIXO**.
- Confirme com o multímetro: **pino 36 do Centronics** (na impressora) ≈ **0 V** com tudo ligado.
- Se estiver alto: o fio GPIO19 → borne 17 → pino 36 não está fechando. Aterre o
  borne 17 direto no GND como reforço.

## 5. O BUSY volta para o ESP32?

Nunca foi confirmado. Se o fio `pino 11 → (1k série ou divisor) → GPIO17` estiver
aberto, o firmware lê `BUSY=0` para sempre e não dá para saber se a impressora respondeu.

- `@PINS` rodando; encoste um jumper do **borne 11 no +5 V** → `BUSY` tem que virar **1**.
  - Não virou → o caminho borne 11 → GPIO17 está aberto. Refaça.
- Depois `pino 11 no GND` → volta a **0**.

## 6. Níveis das entradas de status (ESP32 não é 5 V-tolerante)

BUSY/PE/SELECT/ERROR/ACK vêm da impressora em **5 V**. GPIO 17/16/35/22/15 **não**
aguentam 5 V direto. Use **~1 kΩ em série** em cada uma (recomendação do Manawyrm)
ou um divisor. GPIO35 é *só entrada* — ok para SELECT.

## 7. Amarrações fixas

- DB25 **14** (/AUTOFEED) → **+5 V** (ou o firmware dirige GPIO23 alto).
- DB25 **17** (/SELECT-IN) → **GND** (ou o firmware dirige GPIO19 baixo).
- DB25 **31** (/INIT) → vem do GPIO21; em repouso fica alto.

---

## Quando pedir ajuda / acelerar

- **Foto nítida da fiação** (shield + protoboard + ESP32) para revisar contra `docs/schematic.svg`.
- **Analisador lógico** (~R$30) em `/STROBE` (13→pino1) e `BUSY` (pino11→17):
  mande `@ST` e veja se há pulso de strobe e resposta de BUSY.
- Alguém revisando **fio por fio** com a tabela do `config.h`.

## Voltar ao firmware normal

```
pio run -e esp32dev -t upload
```
