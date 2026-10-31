#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <stdbool.h>
#include <time.h>

#define NUM_CABINES 5
#define NUM_ELEITORES 50
#define NUM_CANDIDATOS 2
#define TAMANHO_BUFFER 10

typedef enum {
    FUNCIONANDO,
    QUEBRADA
} EstadoUrna;

typedef struct {
    int id;
    EstadoUrna estado;
    int votos_registrados;
    pthread_mutex_t mutex_urna;
} Urna;

typedef struct {
    int id_eleitor;
    int candidato_escolhido;
} Voto;

Urna urnas[NUM_CABINES];
int votos_totais[NUM_CANDIDATOS];

Voto buffer_votos[TAMANHO_BUFFER];
int count_buffer = 0;
int in = 0, out = 0;

sem_t semaforo_cabines;

pthread_mutex_t mutex_buffer;
pthread_cond_t cond_buffer_vazio;
pthread_cond_t cond_buffer_cheio;

pthread_mutex_t mutex_apuracao;

pthread_mutex_t mutex_tecnico;
pthread_cond_t cond_tecnico_acionado;
int urnas_quebradas = 0;
bool eleicao_ativa = true;

void* thread_tecnico(void* arg) {
    while (1) {
        pthread_mutex_lock(&mutex_tecnico);
        while (urnas_quebradas == 0 && eleicao_ativa) {
            pthread_cond_wait(&cond_tecnico_acionado, &mutex_tecnico);
        }
        
        if (!eleicao_ativa && urnas_quebradas == 0) {
            pthread_mutex_unlock(&mutex_tecnico);
            break;
        }
        pthread_mutex_unlock(&mutex_tecnico);

        for (int id = 0; id < NUM_CABINES; id++) {
            pthread_mutex_lock(&urnas[id].mutex_urna);
            
            if (urnas[id].estado == QUEBRADA) {
                printf("[TECNICO] Chegou na urna %d. Iniciando conserto...\n", id);
                usleep(500000); 
                urnas[id].estado = FUNCIONANDO;
                printf("[TECNICO] Urna %d consertada e liberada para a fila.\n", id);
                
                pthread_mutex_lock(&mutex_tecnico);
                urnas_quebradas--;
                pthread_mutex_unlock(&mutex_tecnico);
            }
            
            pthread_mutex_unlock(&urnas[id].mutex_urna);
        }
    }
    return NULL;
}

void* thread_rede(void* arg) {
    int processados = 0;
    while (processados < NUM_ELEITORES) {
        pthread_mutex_lock(&mutex_buffer);
        while (count_buffer == 0) {
            pthread_cond_wait(&cond_buffer_vazio, &mutex_buffer);
        }

        Voto v = buffer_votos[out];
        out = (out + 1) % TAMANHO_BUFFER;
        count_buffer--;

        pthread_cond_signal(&cond_buffer_cheio);
        pthread_mutex_unlock(&mutex_buffer);

        pthread_mutex_lock(&mutex_apuracao);
        votos_totais[v.candidato_escolhido]++;
        processados++;
        pthread_mutex_unlock(&mutex_apuracao);
        
        usleep(10000);
    }
    return NULL;
}

void* thread_eleitor(void* arg) {
    int id_eleitor = *(int*)arg;
    free(arg);

    srand(time(NULL) ^ id_eleitor);

    sem_wait(&semaforo_cabines);

    int id_urna = -1;
    
    while (id_urna == -1) {
        for (int i = 0; i < NUM_CABINES; i++) {
            if (pthread_mutex_trylock(&urnas[i].mutex_urna) == 0) {
                
                if (urnas[i].estado == FUNCIONANDO) {
                    id_urna = i;
                    break;
                } else {
                    pthread_mutex_unlock(&urnas[i].mutex_urna);
                }
            }
        }
        if (id_urna == -1) {
            usleep(10000); 
        }
    }

    if (rand() % 100 < 5) {
        printf("[ALERTA] Urna %d apresentou falha com o Eleitor %02d.\n", id_urna, id_eleitor);
        
        urnas[id_urna].estado = QUEBRADA;
        
        pthread_mutex_lock(&mutex_tecnico);
        urnas_quebradas++;
        pthread_cond_signal(&cond_tecnico_acionado);
        pthread_mutex_unlock(&mutex_tecnico);

        while (urnas[id_urna].estado == QUEBRADA) {
            pthread_mutex_unlock(&urnas[id_urna].mutex_urna);
            usleep(50000);
            pthread_mutex_lock(&urnas[id_urna].mutex_urna);
        }
    }
    
    int tempo_votacao = (rand() % 100 + 20) * 1000; 
    usleep(tempo_votacao); 

    Voto v;
    v.id_eleitor = id_eleitor;
    v.candidato_escolhido = rand() % NUM_CANDIDATOS;

    printf("[VOTO] Eleitor %02d depositou seu voto na urna %d (Candidato %d) em %d ms\n", 
           id_eleitor, id_urna, v.candidato_escolhido, tempo_votacao / 1000);

    urnas[id_urna].votos_registrados++;

    pthread_mutex_unlock(&urnas[id_urna].mutex_urna);
    sem_post(&semaforo_cabines);

    pthread_mutex_lock(&mutex_buffer);
    while (count_buffer == TAMANHO_BUFFER) {
        pthread_cond_wait(&cond_buffer_cheio, &mutex_buffer);
    }

    buffer_votos[in] = v;
    in = (in + 1) % TAMANHO_BUFFER;
    count_buffer++;

    pthread_cond_signal(&cond_buffer_vazio);
    pthread_mutex_unlock(&mutex_buffer);

    return NULL;
}

int main() {
    srand(time(NULL));

    sem_init(&semaforo_cabines, 0, NUM_CABINES);
    
    pthread_mutex_init(&mutex_buffer, NULL);
    pthread_cond_init(&cond_buffer_vazio, NULL);
    pthread_cond_init(&cond_buffer_cheio, NULL);
    
    pthread_mutex_init(&mutex_apuracao, NULL);
    
    pthread_mutex_init(&mutex_tecnico, NULL);
    pthread_cond_init(&cond_tecnico_acionado, NULL);

    for (int i = 0; i < NUM_CABINES; i++) {
        urnas[i].id = i;
        urnas[i].estado = FUNCIONANDO;
        urnas[i].votos_registrados = 0;
        pthread_mutex_init(&urnas[i].mutex_urna, NULL);
    }

    pthread_t t_rede;
    pthread_create(&t_rede, NULL, thread_rede, NULL);

    pthread_t t_tecnico;
    pthread_create(&t_tecnico, NULL, thread_tecnico, NULL);

    pthread_t t_eleitores[NUM_ELEITORES];
    for (int i = 0; i < NUM_ELEITORES; i++) {
        int* id = malloc(sizeof(int));
        *id = i;
        pthread_create(&t_eleitores[i], NULL, thread_eleitor, id);
    }

    for (int i = 0; i < NUM_ELEITORES; i++) {
        pthread_join(t_eleitores[i], NULL);
    }

    pthread_join(t_rede, NULL);

    pthread_mutex_lock(&mutex_tecnico);
    eleicao_ativa = false;
    pthread_cond_signal(&cond_tecnico_acionado);
    pthread_mutex_unlock(&mutex_tecnico);
    
    pthread_join(t_tecnico, NULL);

    printf("\n[RESULTADOS FINAIS]\n");
    for(int i = 0; i < NUM_CANDIDATOS; i++) {
        printf("Candidato %d: %d votos - %.2f%%\n", i, votos_totais[i], (float)votos_totais[i] / (float)NUM_ELEITORES * 100);
    }
    printf("\n[INFORMACOES]\n");
    for(int i = 0; i < NUM_CABINES; i++) {
        printf("Urna %d processou %d votos.\n", i, urnas[i].votos_registrados);
    }

    return 0;
}