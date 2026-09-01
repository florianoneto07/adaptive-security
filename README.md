# Adaptive Security Prototype

Protótipo de um framework de **segurança adaptativa** capaz de selecionar entre
diferentes mecanismos de proteção de canal — **TLS 1.3**, **DTLS 1.3** e, em
etapa futura, **OSCORE** — de acordo com o contexto de comunicação.

O objetivo de longo prazo é integrar este motor adaptativo a um projeto de
**AKMA (Authentication and Key Management for Applications) para 5G**.

## Estrutura do repositório

```
adaptive-security-prototype/
├── server/
│   ├── tls_server.c        # servidor TLS 1.3 sobre TCP
│   └── dtls_server.c       # servidor DTLS 1.3 sobre UDP
├── client/
│   ├── tls_client.c        # cliente TLS 1.3 sobre TCP
│   └── dtls_client.c       # cliente DTLS 1.3 sobre UDP
├── common/
│   └── adaptive_security.h # helpers compartilhados (identidade do peer, erros)
├── scripts/
│   ├── generate_certs.sh   # gera CA e certificado de servidor de teste
│   └── run_local_test.sh   # teste E2E em loopback, com casos negativos
├── certs/                  # material criptográfico local (ignorado pelo git)
├── Makefile
├── README.md
└── STATUS.md
```

## Estado atual

| Mecanismo | Situação |
| --- | --- |
| TLS 1.3 | Implementado e validado E2E entre as duas VMs |
| DTLS 1.3 | Código-base preparado, **pendente de validação E2E** |
| OSCORE | Não iniciado |
| Motor adaptativo | Não iniciado |

Detalhes em [STATUS.md](STATUS.md).

## Topologia de teste

| Item | Valor |
| --- | --- |
| VM servidor | `192.168.237.128` |
| VM cliente | `192.168.237.129` |
| TLS | TCP/4433 |
| DTLS | UDP/4444 |

## Dependências

- **wolfSSL** compilado com TLS 1.3 e DTLS 1.3 habilitados
- `gcc`/`clang`, `make`, `openssl`

Build de referência do wolfSSL:

```bash
./configure \
  --enable-tls13 \
  --enable-dtls \
  --enable-dtls13 \
  --enable-opensslextra \
  --enable-sni \
  --enable-keylog-export \
  C_EXTRA_FLAGS="-DWOLFSSL_IP_ALT_NAME -DWOLFSSL_DTLS_CID"
make -j"$(nproc)"
sudo make install
sudo ldconfig
```

Duas flags não são opcionais para este protótipo:

- `--enable-opensslextra` expõe `X509_VERIFY_PARAM_set1_ip_asc()`, usada para
  amarrar o certificado ao IP do servidor.
- `-DWOLFSSL_IP_ALT_NAME` faz o wolfSSL comparar `subjectAltName` do tipo
  `iPAddress`. Sem ela o SAN de IP do certificado é ignorado na validação.

## 1. Gerar certificados

Na raiz do projeto:

```bash
./scripts/generate_certs.sh
```

O script aceita o IP do servidor como argumento (padrão `192.168.237.128`), que
é gravado como `subjectAltName` do certificado:

```bash
./scripts/generate_certs.sh 10.0.0.5
```

O primeiro argumento é o endereço principal; os seguintes viram SANs extras,
classificados automaticamente como IP ou DNS:

```bash
./scripts/generate_certs.sh 192.168.237.128 127.0.0.1 localhost
```

Todo endereço pelo qual o servidor for alcançado precisa estar no
`subjectAltName`, porque os clientes exigem que o endereço passado na linha de
comando conste do certificado (ver *Verificação de identidade* abaixo).

Copie a pasta `certs/` para ambas as VMs. O cliente precisa de `ca.crt`; o
servidor usa `server.crt` e `server.key`. O diretório pode ser trocado pela
variável de ambiente `CERT_DIR`, o que permite rodar os binários de fora da
raiz do repositório.

> A pasta `certs/` é ignorada pelo git. Nenhuma chave privada deve ser
> versionada, mesmo sendo material de laboratório.

## 2. Compilar

```bash
make
```

Os binários são gerados em `build/`. Se o wolfSSL não estiver em um prefixo
conhecido pelo `pkg-config`, aponte-o com `WOLFSSL_DIR`:

```bash
make WOLFSSL_DIR=/opt/wolfssl
```

## 3. Executar

Por padrão os binários resolvem `certs/` **relativamente ao diretório de
trabalho**, então execute-os a partir da raiz do repositório — ou aponte
`CERT_DIR` para outro lugar.

### TLS 1.3

```bash
# VM servidor
./build/tls_server 4433
```

```bash
# VM cliente
./build/tls_client 192.168.237.128 4433
```

Resultado esperado: handshake TLS 1.3 concluído, certificado do servidor
validado contra a CA de teste e troca bidirecional de mensagens
(cipher observado: `TLS_AES_256_GCM_SHA384`).

### DTLS 1.3

```bash
# VM servidor
./build/dtls_server 4444
```

```bash
# VM cliente
./build/dtls_client 192.168.237.128 4444
```

O servidor DTLS descobre o peer espiando o primeiro datagrama com `MSG_PEEK`
antes de conectar o socket UDP — UDP não tem `accept()`, e sem isso as
respostas do handshake não têm destino. Em DTLS 1.3 também é enviado um cookie
no `HelloRetryRequest`, que obriga o cliente a provar que recebe no endereço de
origem que alega ter.

## 4. Verificação de identidade

Validar a cadeia contra a CA responde *"este certificado foi emitido por quem
eu confio"*. Não responde *"este certificado é de quem estou falando"*. São
perguntas diferentes: sem a segunda, qualquer certificado assinado pela mesma
CA é aceito, e um peer interno consegue se passar por outro.

Por isso os clientes exigem que o endereço passado na linha de comando conste
do `subjectAltName` do certificado:

- endereço IP → `X509_VERIFY_PARAM_set1_ip_asc()`, contra SANs `iPAddress`;
- hostname → `wolfSSL_check_domain_name()`, contra SANs `dNSName`.

A lógica fica em [common/adaptive_security.h](common/adaptive_security.h), em
um ponto só, para não divergir entre o cliente TLS e o cliente DTLS.

## 5. Testes

```bash
make
./scripts/run_local_test.sh
```

Sobe os dois pares cliente/servidor em loopback e verifica quatro casos: os
handshakes TLS e DTLS devem fechar, e ambos devem **rejeitar** um certificado
com cadeia válida porém emitido para outro endereço. Os testes negativos são o
que realmente comprova a verificação de identidade — sem eles, um bug que
desligue a checagem passa despercebido.

## Roteiro

1. Validar DTLS 1.3 E2E entre as duas VMs.
2. Coletar métricas comparativas TLS vs DTLS (handshake, RTT, overhead).
3. Adicionar CoAP + OSCORE.
4. Integrar os três mecanismos ao motor de decisão adaptativo.
5. Integrar o motor adaptativo ao projeto de AKMA para 5G.
