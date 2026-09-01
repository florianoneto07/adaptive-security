# Testes de validação

Passo a passo para validar o protótipo do zero, protocolo por protocolo.

Todas as saídas mostradas aqui foram **copiadas de execuções reais**, não
redigidas à mão. Se a sua saída divergir, algo está diferente — a seção
[Diagnóstico](#diagnóstico) cobre as causas mais comuns.

## Índice

- [Antes de começar](#antes-de-começar)
- [Preparação](#preparação)
- [TLS 1.3](#tls-13)
- [DTLS 1.3](#dtls-13)
- [OSCORE](#oscore)
- [Suítes automatizadas](#suítes-automatizadas)
- [Diagnóstico](#diagnóstico)

---

## Antes de começar

Existem dois cenários. Comece pelo primeiro:

| Cenário | Para quê |
| --- | --- |
| **Uma máquina (loopback)** | Validar a aplicação após clonar o repositório. Não precisa de segunda VM. |
| **Duas máquinas** | Validar sobre rede real, com MTU e perda de datagramas de verdade. |

Os comandos marcados com `[SERVIDOR]` e `[CLIENTE]` só diferem no cenário de
duas máquinas. No loopback, tudo roda no mesmo terminal.

### Requisitos

- Linux com `gcc`, `make`, `git`, `openssl`
- wolfSSL com TLS 1.3, DTLS 1.3, AES-CCM e verificação de SAN de IP

Se ainda não tem o wolfSSL:

```bash
sudo apt update && sudo apt install -y build-essential autoconf automake libtool pkg-config git
```

```bash
./scripts/setup_wolfssl.sh "$HOME/.local"
```

O script usa as flags corretas. Instalar em `~/.local` dispensa `sudo`, mas
**exige `LD_LIBRARY_PATH` em toda execução**. Para não repetir:

```bash
echo 'export LD_LIBRARY_PATH=$HOME/.local/lib:${LD_LIBRARY_PATH:-}' >> ~/.bashrc && source ~/.bashrc
```

Se instalou o wolfSSL em `/usr/local` (com `sudo`), ignore o `LD_LIBRARY_PATH`
e o `WOLFSSL_DIR` nos comandos abaixo.

---

## Preparação

### 1. Compilar

```bash
make clean && make WOLFSSL_DIR="$HOME/.local"
```

Esperado: nenhuma advertência, e quatro binários em `build/`.

```bash
ls build/
```

```
dtls_client  dtls_server  tls_client  tls_server
```

### 2. Gerar os certificados

O primeiro argumento é o endereço principal; os seguintes são SANs adicionais.
**Todo endereço pelo qual o servidor for alcançado precisa estar aqui** — os
clientes recusam certificados que não cubram o endereço usado na conexão.

Loopback:

```bash
./scripts/generate_certs.sh 127.0.0.1 localhost
```

Duas máquinas (use o IP real do servidor):

```bash
./scripts/generate_certs.sh [IP_DO_SERVIDOR] 127.0.0.1 localhost
```

Confira o resultado:

```bash
openssl x509 -in certs/server.crt -noout -ext subjectAltName
```

```
X509v3 Subject Alternative Name:
    IP Address:127.0.0.1, DNS:localhost
```

### 3. `[CLIENTE]` Levar a CA para a outra máquina

Só o `ca.crt`. A chave privada **nunca** sai do servidor:

```bash
scp certs/ca.crt [USUARIO]@[IP_DO_CLIENTE]:~/adaptive-security/certs/ca.crt
```

---

## TLS 1.3

Canal sobre TCP, porta 4433 por padrão.

### T1 — Handshake e troca de mensagens

`[SERVIDOR]` deixe rodando (o `-k` mantém no ar entre execuções do cliente):

```bash
./build/tls_server -k 4433
```

`[CLIENTE]` noutro terminal:

```bash
./build/tls_client 127.0.0.1 4433
```

Saída esperada no **cliente**:

```
Conexão TCP estabelecida com 127.0.0.1:4433
Handshake TLS concluído.
Versão: TLSv1.3
Cipher: TLS_AES_256_GCM_SHA384
Certificado do servidor validado (cadeia + identidade 127.0.0.1).
Mensagem enviada (50 bytes): Mensagem enviada pelo cliente em C usando TLS 1.3.
Resposta recebida (42 bytes): Mensagem recebida com sucesso via TLS 1.3.
```

Saída esperada no **servidor**:

```
Servidor TLS aguardando conexão na porta 4433...
Conexão TCP recebida de 127.0.0.1:33832
Handshake TLS concluído.
Versão: TLSv1.3
Cipher: TLS_AES_256_GCM_SHA384
Mensagem recebida (50 bytes): Mensagem enviada pelo cliente em C usando TLS 1.3.
Confirmação enviada (42 bytes).
```

**Critério:** `Versão: TLSv1.3`, cipher negociado, e a resposta chegando de
volta. O código de saída do cliente é `0`.

> A porta de origem (`33832`) muda a cada execução. É esperado.

### T2 — Conexão por hostname

Exercita a validação por nome, e não por IP:

```bash
./build/tls_client localhost 4433
```

Esperado: mesma saída de T1, com `identidade localhost`. Só funciona se
`localhost` estiver no `subjectAltName` (passo 2 da preparação).

### T3 — Certificado emitido para outro endereço *(deve falhar)*

Este é o teste que realmente valida a segurança. Um certificado de **cadeia
válida** mas emitido para outro endereço precisa ser recusado.

Gere um certificado impostor e suba o servidor com ele:

```bash
CERT_DIR=/tmp/impostor ./scripts/generate_certs.sh 198.51.100.7
```

```bash
CERT_DIR=/tmp/impostor ./build/tls_server 4433
```

`[CLIENTE]` — repare que ele confia na CA que assinou o impostor, então a
rejeição só pode vir da checagem de identidade:

```bash
CERT_DIR=/tmp/impostor ./build/tls_client 127.0.0.1 4433
```

Esperado:

```
Conexão TCP estabelecida com 127.0.0.1:4433
Falha no handshake TLS: peer ip address mismatch
```

**Critério:** código de saída `1` e a mensagem `peer ip address mismatch`.
Se este teste **passar** o handshake, a validação de identidade está quebrada.

### T4 — Hostname fora do subjectAltName *(deve falhar)*

Certificado só com SAN de IP, cliente conectando por nome:

```bash
CERT_DIR=/tmp/iponly ./scripts/generate_certs.sh 127.0.0.1
```

```bash
CERT_DIR=/tmp/iponly ./build/tls_server 4433
```

```bash
CERT_DIR=/tmp/iponly ./build/tls_client localhost 4433
```

Esperado:

```
Conexão TCP estabelecida com localhost:4433
Falha no handshake TLS: peer subject name mismatch
```

### T5 — CA desconhecida *(deve falhar)*

Cliente com uma CA que não assinou o certificado do servidor. Monte um
diretório com o certificado legítimo do servidor, mas a CA do impostor:

```bash
mkdir -p /tmp/ca-errada
cp certs/server.crt certs/server.key /tmp/ca-errada/
cp /tmp/impostor/ca.crt /tmp/ca-errada/ca.crt
```

```bash
CERT_DIR=/tmp/ca-errada ./build/tls_server 4433
```

```bash
CERT_DIR=/tmp/ca-errada ./build/tls_client 127.0.0.1 4433
```

Esperado:

```
Conexão TCP estabelecida com 127.0.0.1:4433
Falha no handshake TLS: certificate verify failed
```

Repare na diferença entre T3 e T5: `certificate verify failed` é falha de
**cadeia**; `peer ip address mismatch` é falha de **identidade**. São
verificações distintas, e T3 existe justamente porque a primeira sozinha não
protege contra um peer interno se passando por outro.

---

## DTLS 1.3

Mesmo modelo sobre UDP, porta 4444 por padrão. Como UDP não tem conexão, o
servidor descobre o peer espiando o primeiro datagrama.

### D1 — Handshake e troca de mensagens

`[SERVIDOR]`:

```bash
./build/dtls_server -k 4444
```

`[CLIENTE]`:

```bash
./build/dtls_client 127.0.0.1 4444
```

Saída esperada no **cliente**:

```
Iniciando handshake DTLS 1.3 com 127.0.0.1:4444...
Handshake DTLS concluído.
Versão: DTLSv1.3
Cipher: TLS_AES_256_GCM_SHA384
Certificado do servidor validado (cadeia + identidade 127.0.0.1).
Mensagem enviada (43 bytes): Mensagem enviada pelo cliente via DTLS 1.3.
Resposta recebida (43 bytes): Mensagem recebida com sucesso via DTLS 1.3.
```

Saída esperada no **servidor**:

```
Servidor DTLS 1.3 aguardando na porta 4444...
Datagrama inicial recebido de 127.0.0.1:51292
Handshake DTLS concluído.
Versão: DTLSv1.3
Cipher: TLS_AES_256_GCM_SHA384
Mensagem recebida (43 bytes): Mensagem enviada pelo cliente via DTLS 1.3.
Confirmação enviada (43 bytes).
```

**Critério:** `Versão: DTLSv1.3`. Se aparecer `DTLSv1.2`, a build do wolfSSL
está sem `--enable-dtls13`.

### D2 — Certificado emitido para outro endereço *(deve falhar)*

```bash
CERT_DIR=/tmp/impostor ./build/dtls_server 4444
```

```bash
CERT_DIR=/tmp/impostor ./build/dtls_client 127.0.0.1 4444
```

Esperado:

```
Iniciando handshake DTLS 1.3 com 127.0.0.1:4444...
Falha no handshake DTLS: peer ip address mismatch
```

### D3 — Associações consecutivas

Verifica que o servidor persistente atende vários clientes em sequência. Com o
`dtls_server -k` rodando:

```bash
for i in 1 2 3 4; do ./build/dtls_client 127.0.0.1 4444 | tail -1; done
```

Esperado: quatro linhas `Resposta recebida (43 bytes): ...`.

No log do servidor aparecem linhas assim:

```
Datagrama descartado: tipo de registro 47, não é handshake.
```

Isso é **esperado e correto**: é o `close_notify` da associação anterior
chegando depois de o socket ser recriado. O servidor o descarta em vez de
tomá-lo por uma nova associação — sem esse filtro, ele tentaria negociar com
um cliente que já foi embora, e a segunda associação falharia.

> O valor `47` não é acidente. Em DTLS 1.3 o alerta trafega cifrado sob o
> *unified header*, cujo primeiro byte cai na faixa `0x20`–`0x3F`. Só o
> ClientHello inicial vai em texto claro, com tipo de registro `22`, e é por
> isso que o filtro aceita apenas esse valor.

---

## OSCORE

> **Ainda não implementado.** A infraestrutura está pronta, o teste E2E não.

O que já existe:

```bash
./scripts/setup_libcoap.sh "$HOME/.local"   # instala libcoap com OSCORE sobre wolfSSL
./scripts/generate_oscore_conf.sh           # gera o par de contextos (RFC 8613)
```

O gerador produz `oscore/server.conf` e `oscore/client.conf` com o mesmo
`master_secret` e os IDs cruzados — o `sender_id` de um lado é o
`recipient_id` do outro. Trocá-los é o erro de configuração mais comum, e ele
se manifesta como falha de decifragem, não como erro de configuração.

O `oscore/` é ignorado pelo git porque os arquivos carregam o segredo
compartilhado. Leve o `client.conf` à outra máquina por canal seguro.

Esta seção será completada com os passos de validação quando o OSCORE estiver
integrado. Acompanhe o [STATUS.md](../STATUS.md).

---

## Suítes automatizadas

Preferíveis para validar tudo de uma vez.

### Loopback, uma máquina

```bash
./scripts/run_local_test.sh
```

Saída real:

```
== Preparando ==
Certificados temporários em /tmp/tmp.ZkXnOXDuDj

== Caminho feliz ==
  PASS  TLS 1.3 handshake + troca de mensagens
        Versão: TLSv1.3
        Cipher: TLS_AES_256_GCM_SHA384
  PASS  DTLS 1.3 handshake + troca de mensagens
        Versão: DTLSv1.3
        Cipher: TLS_AES_256_GCM_SHA384
  PASS  TLS por hostname (SAN dNSName)

== Identidade do peer (devem ser REJEITADOS) ==
  PASS  TLS rejeita certificado emitido para outro endereço
        Falha no handshake TLS: peer ip address mismatch
  PASS  DTLS rejeita certificado emitido para outro endereço
        Falha no handshake DTLS: peer ip address mismatch
  PASS  TLS rejeita hostname ausente do subjectAltName
        Falha no handshake TLS: peer subject name mismatch

== Resultado ==
  6 passaram, 0 falharam
```

Código de saída `0` se tudo passou. Usa certificados em diretório temporário,
sem tocar nos seus `certs/`.

### Entre as duas máquinas

Exige SSH sem senha do servidor para o cliente, e o repositório compilado dos
dois lados:

```bash
./scripts/run_remote_test.sh [HOST_SSH_DO_CLIENTE] [IP_DO_SERVIDOR]
```

`[HOST_SSH_DO_CLIENTE]` é um alias do `~/.ssh/config` ou
`[USUARIO]@[IP_DO_CLIENTE]`. `[IP_DO_SERVIDOR]` é o endereço desta máquina
como o cliente a enxerga, e precisa constar do `subjectAltName`.

---

## Diagnóstico

| Sintoma | Causa | Correção |
| --- | --- | --- |
| `connect: Connection refused` | Nenhum servidor escutando. Sem `-k` o servidor sai após uma conexão. | Suba o servidor com `-k`, ou reinicie-o antes de cada cliente. |
| `peer ip address mismatch` | O certificado não cobre o endereço usado. | Regenere incluindo o endereço: `./scripts/generate_certs.sh <IP> 127.0.0.1 localhost` |
| `peer subject name mismatch` | Hostname fora do `subjectAltName`. | Acrescente o nome ao gerar os certificados. |
| `certificate verify failed` | O `ca.crt` do cliente não é o da CA que assinou. | Recopie o `ca.crt` do servidor. |
| `error while loading shared libraries: libwolfssl.so` | wolfSSL fora de `/usr/local`. | `export LD_LIBRARY_PATH=$HOME/.local/lib` |
| `DTLS 1.3 não disponível nesta build` | wolfSSL sem `--enable-dtls13`. | `./scripts/setup_wolfssl.sh "$HOME/.local"` |
| `Build do wolfSSL sem OPENSSL_EXTRA` | Falta `--enable-opensslextra`. | Idem acima. |
| `Erro ao carregar certs/server.crt` | Certificados não gerados, ou execução fora da raiz do repositório. | Rode `./scripts/generate_certs.sh`, ou aponte `CERT_DIR`. |
| Timeout no DTLS, sem mensagem | Datagramas bloqueados no caminho. | Libere UDP/4444 no firewall das duas pontas. |

### Verificações úteis

Há servidor escutando?

```bash
ss -tlnp | grep 4433; ss -ulnp | grep 4444
```

O que a build do wolfSSL habilitou?

```bash
grep -E "define (WOLFSSL_TLS13|WOLFSSL_DTLS13|WOLFSSL_IP_ALT_NAME|OPENSSL_EXTRA|HAVE_AESCCM)$" "$HOME/.local/include/wolfssl/options.h"
```

A cadeia do certificado fecha?

```bash
openssl verify -CAfile certs/ca.crt certs/server.crt
```
