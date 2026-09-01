# Roteiro de testes

Comandos exatos para levantar e testar o protótipo entre as duas VMs.

| Papel | Endereço | Prefixo do wolfSSL |
| --- | --- | --- |
| Servidor | `192.168.218.128` | `~/.local` |
| Cliente | `192.168.218.129` | `~/.local` |

Como o wolfSSL não está em `/usr/local`, **toda execução precisa de
`LD_LIBRARY_PATH`**. Para não repetir isso em todo comando, coloque no
`~/.bashrc` das duas VMs:

```bash
echo 'export LD_LIBRARY_PATH=$HOME/.local/lib:${LD_LIBRARY_PATH:-}' >> ~/.bashrc
```

Os comandos abaixo trazem a variável explícita, para funcionarem mesmo sem isso.

---

## Por que "Connection refused"

Os servidores **encerram após atender uma conexão**. Rodar o cliente sem ter
subido o servidor antes — ou rodá-lo duas vezes seguidas contra um servidor
sem `-k` — dá exatamente esse erro.

Use `-k` para o servidor ficar no ar atendendo conexões seguidas:

```bash
./build/tls_server -k 4433
```

Sem `-k`, o comportamento é o antigo: atende uma e sai.

---

## 1. Compilar (nas duas VMs)

```bash
cd ~/adaptive-security && git pull
```

```bash
make clean && make WOLFSSL_DIR="$HOME/.local"
```

## 2. Gerar certificados (só no SERVIDOR)

Todo endereço pelo qual o servidor for alcançado precisa estar no
`subjectAltName`, senão o cliente recusa a conexão:

```bash
cd ~/adaptive-security && ./scripts/generate_certs.sh 192.168.218.128 127.0.0.1 localhost
```

## 3. Enviar só a CA para o cliente

A chave privada **não** sai do servidor:

```bash
scp ~/adaptive-security/certs/ca.crt woca@192.168.218.129:~/adaptive-security/certs/ca.crt
```

---

## 4. Teste TLS 1.3

**No SERVIDOR**, deixe rodando:

```bash
cd ~/adaptive-security && LD_LIBRARY_PATH=$HOME/.local/lib ./build/tls_server -k 4433
```

**No CLIENTE**, noutro terminal:

```bash
cd ~/adaptive-security && LD_LIBRARY_PATH=$HOME/.local/lib ./build/tls_client 192.168.218.128 4433
```

Esperado no cliente:

```
Handshake TLS concluído.
Versão: TLSv1.3
Cipher: TLS_AES_256_GCM_SHA384
Certificado do servidor validado (cadeia + identidade 192.168.218.128).
```

## 5. Teste DTLS 1.3

**No SERVIDOR**:

```bash
cd ~/adaptive-security && LD_LIBRARY_PATH=$HOME/.local/lib ./build/dtls_server -k 4444
```

**No CLIENTE**:

```bash
cd ~/adaptive-security && LD_LIBRARY_PATH=$HOME/.local/lib ./build/dtls_client 192.168.218.128 4444
```

Esperado: o mesmo, com `Versão: DTLSv1.3`.

---

## 6. Teste automatizado entre as VMs

Do SERVIDOR, sem precisar de terminal no cliente:

```bash
cd ~/adaptive-security && ./scripts/run_remote_test.sh
```

Sobe cada servidor, dispara o cliente remoto por SSH e confere os resultados,
incluindo os casos negativos de identidade.

## 7. Teste local em loopback

Só no servidor, sem depender da outra VM:

```bash
cd ~/adaptive-security && ./scripts/run_local_test.sh
```

---

## Diagnóstico

**`Connection refused`** — não há servidor escutando. Confirme:

```bash
ss -tlnp | grep 4433; ss -ulnp | grep 4444
```

**`peer ip address mismatch`** — o certificado do servidor não cobre o endereço
que o cliente usou. Regenere incluindo esse endereço (passo 2).

**`ASN no signer error to confirm failure`** — o `ca.crt` do cliente não
corresponde à CA que assinou o certificado do servidor. Refaça o passo 3.

**`error while loading shared libraries: libwolfssl.so`** — faltou
`LD_LIBRARY_PATH`.

**`DTLS 1.3 não disponível nesta build`** — wolfSSL sem `--enable-dtls13`.
Rode `./scripts/setup_wolfssl.sh "$HOME/.local"`.
