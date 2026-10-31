/*
 * urnas.c
 *
 * Simulação de uma eleição com urnas eletrônicas usando programação
 * concorrente (POSIX Threads).
 *
 * Atores (threads):
 *   - Eleitores (NUM_ELEITORES): disputam uma urna, votam e depositam o voto
 *     em um buffer compartilhado (produtores).
 *   - Thread de rede (1): retira os votos do buffer e faz a apuração
 *     (consumidora).
 *   - Técnico (1): conserta as urnas que quebram durante a votação.
 *
 * Mecanismos de sincronização:
 *   - Semáforo contador: limita os eleitores simultâneos ao nº de cabines.
 *   - Mutex por urna: uso exclusivo de cada urna.
 *   - Mutex + variáveis de condição: buffer circular (produtor/consumidor)
 *     e acionamento do técnico.
 *   - Mutex de apuração: protege a contagem de votos por candidato.
 *
 * Compilação: gcc -o urnas urnas.c -pthread
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <stdbool.h>
#include <time.h>

#define NUM_CABINES 5      /* nº de urnas/cabines disponíveis */
#define NUM_ELEITORES 50   /* nº de eleitores (uma thread cada) */
#define NUM_CANDIDATOS 2   /* nº de candidatos na eleição */
#define TAMANHO_BUFFER 10  /* capacidade do buffer circular de votos */

/* Estado de uma urna: pode votar (FUNCIONANDO) ou aguarda conserto (QUEBRADA). */
typedef enum {
    FUNCIONANDO,
    QUEBRADA
} EstadoUrna;

/* Representa uma urna eletrônica. */
typedef struct {
    int id;                    /* identificador da urna */
    EstadoUrna estado;         /* FUNCIONANDO ou QUEBRADA */
    int votos_registrados;     /* quantos votos esta urna processou */
    pthread_mutex_t mutex_urna;/* garante uso exclusivo da urna e protege seus campos */
} Urna;

/* Voto depositado por um eleitor, enviado do buffer para a apuração. */
typedef struct {
    int id_eleitor;            /* quem votou */
    int candidato_escolhido;   /* índice do candidato escolhido (0..NUM_CANDIDATOS-1) */
} Voto;

Urna urnas[NUM_CABINES];             /* conjunto de urnas */
int votos_totais[NUM_CANDIDATOS];    /* apuração: total de votos por candidato */

/* Buffer circular de votos (entre eleitores e thread de rede). */
Voto buffer_votos[TAMANHO_BUFFER];
int count_buffer = 0;                /* nº de votos atualmente no buffer */
int in = 0, out = 0;                 /* índices de inserção (in) e remoção (out) */

sem_t semaforo_cabines;              /* conta as cabines livres (valor inicial = NUM_CABINES) */

pthread_mutex_t mutex_buffer;        /* protege buffer_votos, in, out e count_buffer */
pthread_cond_t cond_buffer_vazio;    /* sinalizada quando entra um voto (acorda o consumidor) */
pthread_cond_t cond_buffer_cheio;    /* sinalizada quando sai um voto (acorda o produtor) */

pthread_mutex_t mutex_apuracao;      /* protege votos_totais */

pthread_mutex_t mutex_tecnico;       /* protege urnas_quebradas e eleicao_ativa */
pthread_cond_t cond_tecnico_acionado;/* acorda o técnico quando há urna quebrada ou fim da eleição */

int urnas_quebradas = 0;             /* nº de urnas aguardando conserto */
bool eleicao_ativa = true;           /* vira false quando todos os eleitores terminaram */

/*
 * Dorme até que alguma urna quebre. Ao ser acordado, percorre as urnas,
 * conserta as quebradas e volta a dormir. Termina quando a eleição acaba e
 * não há mais urnas quebradas.
 */
void* thread_tecnico(void* arg) {
    while (1) {
        pthread_mutex_lock(&mutex_tecnico);

        /* Espera (sem gastar CPU) enquanto não há trabalho e a eleição segue.
         * O while protege contra despertares espúrios. */
        while (urnas_quebradas == 0 && eleicao_ativa) {
            pthread_cond_wait(&cond_tecnico_acionado, &mutex_tecnico);
        }

        /* Eleição encerrada e nada a consertar: o técnico pode ir embora. */
        if (!eleicao_ativa && urnas_quebradas == 0) {
            pthread_mutex_unlock(&mutex_tecnico);
            break;
        }
        pthread_mutex_unlock(&mutex_tecnico);

        /* Percorre todas as urnas procurando as que estão quebradas. */
        for (int id = 0; id < NUM_CABINES; id++) {
            /* O eleitor libera o mutex da urna enquanto espera o conserto,
             * o que permite ao técnico obtê-lo aqui. */
            pthread_mutex_lock(&urnas[id].mutex_urna);
            if (urnas[id].estado == QUEBRADA) {
                printf("[TECNICO] Chegou na urna %d. Iniciando conserto...\n", id);
                usleep(500000); /* simula 500 ms de conserto */
                urnas[id].estado = FUNCIONANDO;
                printf("[TECNICO] Urna %d consertada e liberada para a fila.\n", id);

                /* Ordem de locks: mutex_urna -> mutex_tecnico (a mesma usada
                 * pelo eleitor), o que evita deadlock. */
                pthread_mutex_lock(&mutex_tecnico);
                urnas_quebradas--;
                pthread_mutex_unlock(&mutex_tecnico);
            }
            pthread_mutex_unlock(&urnas[id].mutex_urna);
        }
    }
    return NULL;
}

/*
 * Consumidora do buffer: retira votos e os contabiliza. Encerra depois de
 * apurar NUM_ELEITORES votos (cada eleitor produz exatamente um).
 */
void* thread_rede(void* arg) {
    int processados = 0;

    while (processados < NUM_ELEITORES) {
        pthread_mutex_lock(&mutex_buffer);

        /* Buffer vazio: espera até que um eleitor deposite um voto. */
        while (count_buffer == 0) {
            pthread_cond_wait(&cond_buffer_vazio, &mutex_buffer);
        }

        /* Retira o voto da posição 'out' (fila circular). */
        Voto v = buffer_votos[out];
        out = (out + 1) % TAMANHO_BUFFER;
        count_buffer--;

        /* Abriu uma vaga: acorda um eleitor que esteja esperando buffer cheio. */
        pthread_cond_signal(&cond_buffer_cheio);
        pthread_mutex_unlock(&mutex_buffer);

        /* Seção crítica da apuração: atualiza o total do candidato. */
        pthread_mutex_lock(&mutex_apuracao);
        votos_totais[v.candidato_escolhido]++;
        processados++;
        pthread_mutex_unlock(&mutex_apuracao);

        usleep(10000); /* simula 10 ms de latência de rede */
    }
    return NULL;
}

/*
 * Ciclo de um eleitor:
 *   1) reserva uma cabine (semáforo);
 *   2) encontra uma urna funcionando e a trava;
 *   3) com 5% de chance a urna quebra e o eleitor espera o conserto;
 *   4) vota e libera urna e cabine;
 *   5) deposita o voto no buffer (produtor).
 */
void* thread_eleitor(void* arg) {
    int id_eleitor = *(int*)arg;
    free(arg); /* o id foi alocado em main; aqui é liberado */

    /* Semente diferente por eleitor para variar os sorteios. */
    srand(time(NULL) ^ id_eleitor);

    /* (1) Espera uma cabine livre: no máximo NUM_CABINES eleitores passam. */
    sem_wait(&semaforo_cabines);

    /* (2) Procura uma urna funcionando sem bloquear (trylock): se uma urna
     * está ocupada ou quebrada, tenta a próxima. Se nenhuma serve, espera
     * 10 ms e repete. */
    int id_urna = -1;
    while (id_urna == -1) {
        for (int i = 0; i < NUM_CABINES; i++) {
            if (pthread_mutex_trylock(&urnas[i].mutex_urna) == 0) {
                if (urnas[i].estado == FUNCIONANDO) {
                    id_urna = i; /* achou: mantém o mutex travado durante o uso */
                    break;
                } else {
                    /* Urna quebrada: solta o mutex e continua a busca. */
                    pthread_mutex_unlock(&urnas[i].mutex_urna);
                }
            }
        }
        if (id_urna == -1) {
            usleep(10000);
        }
    }

    /* (3) Falha aleatória: 5% de chance de a urna quebrar neste uso. */
    if (rand() % 100 < 5) {
        printf("[ALERTA] Urna %d apresentou falha com o Eleitor %02d.\n", id_urna, id_eleitor);
        urnas[id_urna].estado = QUEBRADA;

        /* Avisa o técnico (mutex_urna já está travado -> mutex_tecnico). */
        pthread_mutex_lock(&mutex_tecnico);
        urnas_quebradas++;
        pthread_cond_signal(&cond_tecnico_acionado);
        pthread_mutex_unlock(&mutex_tecnico);

        /* Espera o conserto. O mutex da urna é solto durante o sono para o
         * técnico conseguir travá-la e consertá-la; depois é retomado para
         * checar o estado. */
        while (urnas[id_urna].estado == QUEBRADA) {
            pthread_mutex_unlock(&urnas[id_urna].mutex_urna);
            usleep(50000);
            pthread_mutex_lock(&urnas[id_urna].mutex_urna);
        }
    }

    /* (4) Votação: tempo aleatório entre 20 ms e 119 ms (em microssegundos). */
    int tempo_votacao = (rand() % 100 + 20) * 1000;
    usleep(tempo_votacao);

    /* Monta o voto com um candidato sorteado. */
    Voto v;
    v.id_eleitor = id_eleitor;
    v.candidato_escolhido = rand() % NUM_CANDIDATOS;

    printf("[VOTO] Eleitor %02d depositou seu voto na urna %d (Candidato %d) em %d ms\n",
           id_eleitor, id_urna, v.candidato_escolhido, tempo_votacao / 1000);

    /* Contabiliza na urna (protegida pelo mutex que o eleitor já detém) e a libera. */
    urnas[id_urna].votos_registrados++;
    pthread_mutex_unlock(&urnas[id_urna].mutex_urna);

    /* Devolve a cabine para o próximo eleitor da fila. */
    sem_post(&semaforo_cabines);

    /* (5) Produtor: insere o voto no buffer circular. */
    pthread_mutex_lock(&mutex_buffer);

    /* Buffer cheio: espera a thread de rede abrir uma vaga. */
    while (count_buffer == TAMANHO_BUFFER) {
        pthread_cond_wait(&cond_buffer_cheio, &mutex_buffer);
    }

    buffer_votos[in] = v;
    in = (in + 1) % TAMANHO_BUFFER;
    count_buffer++;

    /* Acorda a thread de rede, caso ela esteja esperando buffer vazio. */
    pthread_cond_signal(&cond_buffer_vazio);
    pthread_mutex_unlock(&mutex_buffer);

    return NULL;
}

int main() {
    srand(time(NULL));

    /* Inicialização dos mecanismos de sincronização. */
    sem_init(&semaforo_cabines, 0, NUM_CABINES); /* 0 = compartilhado entre threads; valor inicial = nº de cabines */
    pthread_mutex_init(&mutex_buffer, NULL);
    pthread_cond_init(&cond_buffer_vazio, NULL);
    pthread_cond_init(&cond_buffer_cheio, NULL);
    pthread_mutex_init(&mutex_apuracao, NULL);
    pthread_mutex_init(&mutex_tecnico, NULL);
    pthread_cond_init(&cond_tecnico_acionado, NULL);

    /* Inicialização das urnas: todas começam funcionando e sem votos. */
    for (int i = 0; i < NUM_CABINES; i++) {
        urnas[i].id = i;
        urnas[i].estado = FUNCIONANDO;
        urnas[i].votos_registrados = 0;
        pthread_mutex_init(&urnas[i].mutex_urna, NULL);
    }

    /* Cria as threads de apoio: apuração (rede) e manutenção (técnico). */
    pthread_t t_rede;
    pthread_create(&t_rede, NULL, thread_rede, NULL);

    pthread_t t_tecnico;
    pthread_create(&t_tecnico, NULL, thread_tecnico, NULL);

    /* Cria uma thread por eleitor. O id é alocado no heap para que cada
     * thread receba seu próprio valor (a thread libera com free). */
    pthread_t t_eleitores[NUM_ELEITORES];
    for (int i = 0; i < NUM_ELEITORES; i++) {
        int* id = malloc(sizeof(int));
        *id = i;
        pthread_create(&t_eleitores[i], NULL, thread_eleitor, id);
    }

    /* Encerramento ordenado: primeiro todos os eleitores... */
    for (int i = 0; i < NUM_ELEITORES; i++) {
        pthread_join(t_eleitores[i], NULL);
    }

    /* ...depois a apuração (que só termina após contar todos os votos)... */
    pthread_join(t_rede, NULL);

    /* ...e por fim avisa o técnico de que a eleição acabou. O sinal acorda o
     * técnico caso ele esteja dormindo à espera de urnas quebradas. */
    pthread_mutex_lock(&mutex_tecnico);
    eleicao_ativa = false;
    pthread_cond_signal(&cond_tecnico_acionado);
    pthread_mutex_unlock(&mutex_tecnico);
    pthread_join(t_tecnico, NULL);

    /* Resultado final: votos e percentual de cada candidato. */
    printf("\n[RESULTADOS FINAIS]\n");
    for(int i = 0; i < NUM_CANDIDATOS; i++) {
        printf("Candidato %d: %d votos - %.2f%%\n", i, votos_totais[i], (float)votos_totais[i] / (float)NUM_ELEITORES * 100);
    }

    /* Quantos votos cada urna processou. */
    printf("\n[INFORMACOES]\n");
    for(int i = 0; i < NUM_CABINES; i++) {
        printf("Urna %d processou %d votos.\n", i, urnas[i].votos_registrados);
    }

    return 0;
}