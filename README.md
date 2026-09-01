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
├── scripts/
│   └── generate_certs.sh   # gera CA e certificado de servidor de teste
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
./configure --enable-tls13 --enable-dtls --enable-dtls13
make -j"$(nproc)"
sudo make install
sudo ldconfig
```

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

Copie a pasta `certs/` para ambas as VMs. O cliente precisa de `ca.crt`; o
servidor usa `server.crt` e `server.key`.

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

Todos os binários resolvem `certs/` **relativamente ao diretório de trabalho**,
portanto execute-os a partir da raiz do repositório.

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

> **Baseline de desenvolvimento, ainda não validado.** Dependendo da build do
> wolfSSL, o DTLS pode exigir configuração adicional de peer, cookie
> (`HelloVerifyRequest`) e timeouts de retransmissão. Trate estes dois arquivos
> como ponto de partida, não como etapa concluída.

## Roteiro

1. Validar DTLS 1.3 E2E entre as duas VMs.
2. Coletar métricas comparativas TLS vs DTLS (handshake, RTT, overhead).
3. Adicionar CoAP + OSCORE.
4. Integrar os três mecanismos ao motor de decisão adaptativo.
5. Integrar o motor adaptativo ao projeto de AKMA para 5G.
