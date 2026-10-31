# Simulação de Urnas Eletrônicas com Programação Concorrente

Simulação de uma eleição em C usando **POSIX Threads (pthreads)**. Eleitores, urnas, um técnico de manutenção e uma thread de rede (apuração) executam de forma concorrente e se coordenam por **semáforos, mutexes e variáveis de condição**.

O projeto demonstra, em um cenário concreto, problemas e padrões clássicos de sincronização: alocação de recursos limitados, produtor/consumidor com buffer circular e proteção de seções críticas.

## Visão geral

Cada eleitor é uma thread que precisa de uma cabine de votação livre. Ao votar, o eleitor deposita seu voto em um buffer compartilhado, de onde a thread de rede o retira para contabilizar na apuração. Eventualmente uma urna pode quebrar durante o voto; nesse caso, a thread do técnico é acionada para consertá-la e devolvê-la à fila.

### Threads

| Thread | Quantidade | Papel |
|---|---|---|
| `thread_eleitor` | 50 | Disputa uma urna, vota (podendo encontrar falha) e produz o voto no buffer |
| `thread_rede` | 1 | Consome votos do buffer e atualiza a apuração |
| `thread_tecnico` | 1 | Dorme até que haja urna quebrada, conserta e libera a urna |
| `main` | 1 | Inicializa os recursos, cria as threads, aguarda o término e imprime os resultados |

### Fluxo de um eleitor

1. Aguarda uma cabine livre no **semáforo** de cabines.
2. Procura uma urna funcionando com `pthread_mutex_trylock`, testando todas até conseguir uma.
3. Com **5% de chance**, a urna falha: ela é marcada como `QUEBRADA`, o técnico é acionado e o eleitor espera o conserto (liberando o mutex enquanto aguarda).
4. Vota (tempo aleatório entre 20 ms e 119 ms) em um dos candidatos, escolhido aleatoriamente.
5. Libera a urna e a cabine.
6. Insere o voto no **buffer circular**, bloqueando caso ele esteja cheio.

## Mecanismos de sincronização

| Recurso | Mecanismo | Finalidade |
|---|---|---|
| `semaforo_cabines` | Semáforo contador (`sem_t`) | Limita o número de eleitores simultâneos ao número de cabines |
| `urnas[i].mutex_urna` | Mutex por urna | Garante uso exclusivo de cada urna (e protege seu estado) |
| `buffer_votos` | Mutex + 2 variáveis de condição (`cond_buffer_vazio` / `cond_buffer_cheio`) | Produtor/consumidor com buffer circular limitado |
| `votos_totais` | `mutex_apuracao` | Protege a contagem dos votos por candidato |
| `urnas_quebradas` | `mutex_tecnico` + `cond_tecnico_acionado` | Acorda o técnico apenas quando há urna quebrada |
| `eleicao_ativa` | Flag protegida por `mutex_tecnico` | Sinaliza o encerramento da eleição ao técnico |

## Parâmetros

Definidos por macros no início de `urnas.c`:

| Macro | Valor padrão | Descrição |
|---|---|---|
| `NUM_CABINES` | `5` | Número de urnas/cabines |
| `NUM_ELEITORES` | `50` | Número de eleitores (threads) |
| `NUM_CANDIDATOS` | `2` | Número de candidatos |
| `TAMANHO_BUFFER` | `10` | Capacidade do buffer circular de votos |

A probabilidade de falha (5%) e o tempo de conserto (500 ms) estão fixos no código, em `thread_eleitor` e `thread_tecnico`.

## Como executar

### Requisitos

- Sistema POSIX (Linux, WSL ou similar)
- `gcc` (ou outro compilador C compatível) com suporte a pthreads

> Observação: o programa usa semáforos POSIX sem nome (`sem_init`), que não são suportados no macOS.

### Compilação

```bash
gcc -o urnas urnas.c -pthread
```

### Execução

```bash
./urnas
```

## Exemplo de saída

A saída varia a cada execução, pois falhas, tempos de votação e escolhas são aleatórios.

```text
[VOTO] Eleitor 03 depositou seu voto na urna 0 (Candidato 1) em 62 ms
[ALERTA] Urna 2 apresentou falha com o Eleitor 11.
[TECNICO] Chegou na urna 2. Iniciando conserto...
[TECNICO] Urna 2 consertada e liberada para a fila.
[VOTO] Eleitor 11 depositou seu voto na urna 2 (Candidato 0) em 87 ms
...

[RESULTADOS FINAIS]
Candidato 0: 27 votos - 54.00%
Candidato 1: 23 votos - 46.00%

[INFORMACOES]
Urna 0 processou 11 votos.
Urna 1 processou 9 votos.
...
```

### Tipos de mensagem

- `[VOTO]`: um eleitor depositou o voto em uma urna.
- `[ALERTA]`: uma urna falhou durante a votação.
- `[TECNICO]`: o técnico iniciou ou concluiu um conserto.
- `[RESULTADOS FINAIS]`: total e percentual de votos por candidato.
- `[INFORMACOES]`: quantidade de votos processados por cada urna.

## Estrutura do repositório

```text
.
├── urnas.c     # Código-fonte da simulação
└── README.md
```

## Tecnologias

- Linguagem C
- POSIX Threads (`pthread.h`)
- Semáforos POSIX (`semaphore.h`)

## Autor

Desenvolvido por [@eriqueto](https://github.com/eriqueto).
