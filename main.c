/*
    Antonio Sergio da Silva Lopes  -  2017262466
    Maria Paula de Alencar Viegas  -  2017125592
    gcc main.c -lpthread -D_REENTRANT -Wall -o prog estruturas.h drone_movement.c drone_movement.h  -lm
    echo "ORDER REQ_1 Prod:A, 5 to: 300, 100" >input_pipe
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/shm.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <errno.h>
#include <sys/types.h>
#include <math.h>
#include <sys/msg.h>
#include <time.h>
#include <pthread.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/time.h>
#include <semaphore.h>

#include "drone_movement.h"
#include "estruturas.h"

#define MAX_BUFFER  1000
#define PIPE_NAME "input_pipe"

//Lista de Encomendas
Encomenda *headListaE;
int id_encomenda = 0;

//informacoes ficheiro
Dados *dados;
Warehouse *armazens;
char mensagem[MAX_BUFFER];

//memoria partilhada
Estats *estatisticas;
Warehouse *armazensShm;
int shmid_estats, shmid_armazens, mutex_shmid;
int sem_shmid;

//pipe
int fd_pipe;

//processos
pid_t idCentral;
pid_t idArmazem;
pid_t processo_central;
pid_t processo_gestor;
pid_t processo_armazem, pidWh;

//threads
pthread_t *my_thread;
Drones *arrayDrones;

//MQ
int mq_id;
int mq_id2;

//semaforos e mutexes
//sem_struct *semaforos;
mutex_struct *mutexes;
pthread_cond_t cond_nao_escolhido = PTHREAD_COND_INITIALIZER;

//funcoes
void generateStock();
int escolheDrone();
void sinal_estatistica();
void sinal_saida (int sig);
void *controla_drone(void *id_ptr);
void write_log(char* mensagem);
void ArmazensProcess();
void criaArmazens(int n);
void central();
//void shm_semaforos();
void shm_mutex();
void criaDrones(int numI, int qtd);
void escolhe_armazem(Encomenda *novoNode);
void escolherDestino(double *buf,double x, double y);

int main() {
    dados = (Dados *) malloc(sizeof(Dados));
    int i = 0;
    long pos;
    char *token;
    char *linha = (char *) malloc(sizeof(char) * MAX_BUFFER);
    char *produto = (char *) malloc(sizeof(char) * 255);
    pid_t novo_processo;
    Warehouse *arrayArmazens;
    int armazemN = 1;
    time_t tempo = time(NULL);
    struct tm *t = localtime(&tempo);


    //inicio do programa, limpa o conteudo existente e guarda no ficheiro log
    processo_gestor = getpid();
    sprintf(mensagem, "%d:%d:%d Inicio do programa [%d]\n",t->tm_hour,t->tm_min,t->tm_sec,getpid());
    printf("%s", mensagem);
    FILE *fpLog = fopen("log.txt","w");
    if(fpLog != NULL){
        fseek(fpLog, 0, SEEK_END);
        fprintf(fpLog,"%s", mensagem);
        fclose(fpLog);
    }
    mensagem[0] = '\0';

    //inicia memoria partilhada
    if ((shmid_estats = shmget(IPC_PRIVATE, sizeof(Estats), IPC_CREAT | 0766)) < 0) {
        printf("Erro no smget\n");
        exit(1);
    }
    if ((estatisticas = (Estats *) shmat(shmid_estats, NULL, 0)) < 0) {
        printf("error no shmat");
        exit(1);
    }
    estatisticas->encomendas_entregues = 0;
    estatisticas->prod_carregados = 0;
    estatisticas->prod_entregues = 0;
    estatisticas->encomendas_atribuidas = 0;
    estatisticas->tempo_medio_individual = 0.0;
    estatisticas->tempo_medio_total = 0.0;

    //leitura do ficheiro
    printf("\n----------Informacoes do ficheiro--------\n");
    FILE *fp = fopen("config.txt", "r");
    if (fp != NULL) {
        //primeira linha do ficheiro
        fscanf(fp, "%d, %d\n", &dados->max_x, &dados->max_y);
        printf("%d   %d\n", dados->max_x, dados->max_y);
        pos = ftell(fp);
        fseek(fp, pos, SEEK_SET); //troca a linha
        //segunda linha do ficheiro
        fgets(linha, MAX_BUFFER, fp); //recebe a linha
        //troca o \n por \0
        linha[strlen(linha) - 1] = '\0';
        token = strtok(linha, ", ");
        while (token != NULL) {
            produto = token;
            //printf("%s\n", produto);
            token = strtok(NULL, ", ");
            dados->tipos_produtos[i] = produto;
            //printf("%s\n", dados->tipos_produtos[i]);
            i++;
        }
        printf("%s    %s     %s\n", dados->tipos_produtos[0], dados->tipos_produtos[1], dados->tipos_produtos[2]);
        //printf("%s\n", dados->tipos_produtos[0]);

        //terceira linha do ficheiro
        fscanf(fp, "%d\n", &dados->n_drones);
        pos = ftell(fp);
        fseek(fp, pos, SEEK_SET); //troca a linha
        printf("%d\n", dados->n_drones);

        //quarta linha do ficheiro
        fscanf(fp, "%d, %d, %d\n", &dados->f_abast, &dados->qtd, &dados->unidadeT);
        pos = ftell(fp);
        fseek(fp, pos, SEEK_SET); //troca a linha
        printf("%d   %d     %d \n", dados->f_abast, dados->qtd, dados->unidadeT);

        //sexta linha do ficheiro
        fscanf(fp, "%d \n", &dados->numWh);
        pos = ftell(fp);
        fseek(fp, pos, SEEK_SET); //troca a linha
        printf("%d\n", dados->numWh);

        //leitura de cada armazem ate o fim do ficheiro
        i = 0;
        arrayArmazens = malloc(sizeof(Warehouse) * dados->numWh);
        while (i < dados->numWh) {
            char *info = (char *) malloc(sizeof(char) * MAX_BUFFER);
            char *nome_prod = (char *) malloc(sizeof(char) * MAX_BUFFER);
            char *qtdI = (char *) malloc(sizeof(char) * MAX_BUFFER);
            char *nome = (char *) malloc(sizeof(char) * MAX_BUFFER);
            int x, y;

            fscanf(fp, "%s xy: %d, %d prod: ", nome, &x, &y);
            printf("%s    %d  %d\n", nome, x, y);
            arrayArmazens[i].nome = nome;
            arrayArmazens[i].coordenadas[0] = x;
            arrayArmazens[i].coordenadas[1] = y;
            arrayArmazens[i].W_NO = i + 1;


            fgets(info, MAX_BUFFER, fp);

            info[strlen(info) - 1] = '\0';

            token = strtok(info, ", ");
            int aux = 0;
            while (token) {
                nome_prod = token;

                token = strtok(NULL, ", ");
                qtdI = token;
                token = strtok(NULL, ", ");

                //guarda o produto e suas informacoes na lista de structprodutos
                struct produtos *nProd = malloc(sizeof(struct produtos));
                nProd->produto = nome_prod;
                nProd->qt = atoi(qtdI);
                arrayArmazens[i].produtos[aux].produto = nProd->produto;
                arrayArmazens[i].produtos[aux].qt = nProd->qt;
                printf("%s    %d  \n", arrayArmazens[i].produtos[aux].produto, arrayArmazens[i].produtos[aux].qt);
                aux++;
            }
            //troca de linha do ficheiro
            pos = ftell(fp);
            fseek(fp, pos, SEEK_SET);
            i++;
            //cria memoria partilhada
        }
    }
    fclose(fp);
    printf("Ficheiro lido.\n");
    printf("-----------------------------------------\n\n");

    //ficheiro lido

    //receção de SIGINT
    signal(SIGINT, sinal_saida);
    signal(SIGUSR1, sinal_estatistica); //Informação das estatisticas
    printf("->Sinais recebidos.\n");

    //inicializa os mutexes
    shm_mutex();

    //inicializa os semaforos
    //shm_semaforos();

    //cria shared mem_armazens
    shmid_armazens = shmget(IPC_PRIVATE, sizeof(Warehouse) * dados->numWh, IPC_CREAT | 0766);
    armazensShm = (Warehouse *) shmat(shmid_armazens, NULL, 0);
    printf("->Memoria partilhada criada.\n");
    //memoria partilhada criada

    for (int k = 0; k < dados->numWh; k++) {
        armazensShm[k].nome = arrayArmazens[k].nome;
        armazensShm[k].coordenadas[0] = arrayArmazens[k].coordenadas[0];
        armazensShm[k].coordenadas[1] = arrayArmazens[k].coordenadas[1];
        armazensShm[k].produtos[0] = arrayArmazens[k].produtos[0];
        armazensShm[k].produtos[1] = arrayArmazens[k].produtos[1];
        armazensShm[k].produtos[2] = arrayArmazens[k].produtos[2];
        armazensShm[k].W_NO = arrayArmazens[k].W_NO;
    }
    printf("->Armazens na Shared Memory.\n");

    //cria fila de mensagens
    if ((mq_id = msgget(IPC_PRIVATE, IPC_CREAT | 0700)) < 0) {
        perror("Erro msgget()");
        return -1;
    } else {
        printf("->MQ criada.\n");
    }
    if ((mq_id2 = msgget(IPC_PRIVATE, IPC_CREAT | 0700)) < 0) {
        perror("Erro msgget()");
        return -1;
    } else {
        printf("->MQ criada.\n");
    }
    //fila de mensagens criada

    //Cria processo central
    novo_processo = processo_gestor;
    novo_processo = fork();
    for (i = 0; i < (dados->numWh + 1); i++) {
        if (i == 0) {
            if (novo_processo == 0) { //guardar na variavel apenas para o processo central
                processo_central = getpid();
                central();
                exit(0);
            }
        } else {
            novo_processo = fork();
            if (novo_processo == 0) {
                criaArmazens(armazemN);
                exit(0);
            }
            armazemN++;
        }
    }
    generateStock();
    sleep(1);
}

//Gera reabastecimento de stock
void generateStock() {
    int i = 1;
    int contador = 1;
    char prods[128][100];
    int f = 0;
    int x = 0;
    while(dados->tipos_produtos[f] != NULL)
    {
        strcpy(prods[f],dados->tipos_produtos[f]);
        x += 1 ;
        f++ ;
    }
    printf("%s\n%s\n%s\n%s\n",prods[0],prods[1],prods[2],prods[3]);
    while(1) {
        if(i%dados->numWh+1 == contador) {
            int k = rand()%x;
            long mtype = contador;
            maisStock atualiza;
            atualiza.mtype = mtype;
            atualiza.num_products = dados->qtd;
            strcpy(atualiza.nome_prod,prods[k]);
            atualiza.comentario = 1;
            msgsnd(mq_id, &atualiza, sizeof(atualiza) - sizeof(long), 0);
            sleep(dados->f_abast/5);
            contador++;
            }
        if(contador > dados->numWh) {
            contador = 1;
        }
        i++ ;
    }
}

/*// Inicializar semáforos em memória partilhada
void shm_semaforos() {
    time_t tempo = time(NULL);
    struct tm *t = localtime(&tempo);
    if((sem_shmid = shmget(IPC_PRIVATE, sizeof(sem_struct*), IPC_CREAT|0700))<0){
        perror("Error - shmget() of semafores struct");
    }

    if((semaforos = (sem_struct*) shmat(sem_shmid, NULL, 0)) == (sem_struct*)-1){
        perror("Error - shmat() of semafores struct");
    }

    // Inicializar
    sem_init(&semaforos->mq1, 1, dados->qyd);
    sem_init(&semaforos->mq2, 0, dados->qtd);

    sprintf(mensagem, "->%d:%d:%d Semaforos criados\n",t->tm_hour,t->tm_min,t->tm_sec);
    printf("%s", mensagem);
    pthread_mutex_lock(&mutexes->write_file);
    write_log(mensagem);
    pthread_mutex_unlock(&mutexes->write_file);
    mensagem[0]='\0';
}*/

// Inicializar mutexes
void shm_mutex() {
    time_t tempo = time(NULL);
    struct tm *t = localtime(&tempo);
    if((mutex_shmid = shmget(IPC_PRIVATE, sizeof(mutex_struct*), IPC_CREAT|0700))<0){
        perror("Error - shmget() of mutexes struct");
    }

    if((mutexes = (mutex_struct*) shmat(mutex_shmid, NULL, 0)) == (mutex_struct*)-1){
        perror("Error - shmat() of mutexes struct");
    }

    // escrita log file
    if(pthread_mutex_init(&mutexes->write_file, NULL) != 0) {
        perror("Error - init() of mutexes->write_file");
    }

    if(pthread_mutex_init(&mutexes->get_queue, NULL) != 0) {
        perror("Error - init() of mutexes->get_queue");
    }

    if(pthread_mutex_init(&mutexes->ctrlc, NULL) != 0) {
        perror("Error - init() of mutexes->ctrlc");
    }

    if(pthread_mutex_init(&mutexes->write_stats, NULL) != 0) {
        perror("Error - init() of mutexes->write_stats");
    }

    if(pthread_mutex_init(&mutexes->retirar_mq, NULL) != 0) {
        perror("Error - init() of mutexes->retirar_mq");
    }

    if(pthread_mutex_init(&mutexes->write_armazens, NULL) != 0) {
        perror("Error - init() of mutexes->write_armazens");
    }

    if(pthread_mutex_init(&mutexes->drones, NULL) != 0) {
        perror("Error - init() of mutexes->drones");
    }

    sprintf(mensagem, "->%d:%d:%d Mutexes criados\n",t->tm_hour,t->tm_min,t->tm_sec);
    printf("%s", mensagem);
    pthread_mutex_lock(&mutexes->write_file);
    write_log(mensagem);
    pthread_mutex_unlock(&mutexes->write_file);
    mensagem[0]='\0';
}

//Atualiza armazens
void criaArmazens(int n) {
    //tempo
    time_t tempo = time(NULL);
    struct tm *t = localtime(&tempo);
    Warehouse aux;

    for (int i = 0; i < dados->numWh; i++) {
        if (armazensShm[i].W_NO == n) {
            aux = armazensShm[i];
        }
    }
    
    pthread_mutex_lock(&mutexes->write_armazens);
    armazensShm[n].pid = getpid();
    pthread_mutex_unlock(&mutexes->write_armazens);
    
    printf("Armazem %s; coordernadas x: %d y: %d, produto : %s qt:%d\n", aux.nome, aux.coordenadas[0],
           aux.coordenadas[1], aux.produtos[0].produto, aux.produtos[0].qt);
    
    sprintf(mensagem, "%d:%d:%d Warehouse%d criada (id = %ld)\n", t->tm_hour, t->tm_min, t->tm_sec, n, (long) getpid());
    write_log(mensagem);
    mensagem[0] = '\0';

    fflush(stdout);
    while (1) {
        maisStock mexeStock;
        if (msgrcv(mq_id, &mexeStock, sizeof(mexeStock) - sizeof(long), n, 0)) {
            if (mexeStock.comentario == 1) {
                for (int i = 0; i < 3; i++) {
                    fflush(stdout);
                    if (strcmp(mexeStock.nome_prod, aux.produtos[i].produto) == 0) {
                        pthread_mutex_lock(&(mutexes->write_armazens));
                        aux.produtos[i].qt = aux.produtos[i].qt + mexeStock.num_products;
                        pthread_mutex_unlock(&(mutexes->write_armazens));
                        sprintf(mensagem, "%d:%d:%d Armazem%d atualizou o stock: %d prod do tipo %s\n",
                            t->tm_hour, t->tm_min, t->tm_sec, n, mexeStock.num_products, mexeStock.nome_prod);
                        printf("----------------------------------------------------------\n%s----------------------------------------------------------\n", mensagem);
                        pthread_mutex_lock(&(mutexes->write_file));
                        write_log(mensagem);
                        pthread_mutex_unlock(&(mutexes->write_file));
                        mensagem[0] = '\0';
                    }
                }
                //printf("Atualizado\n");
                fflush(stdout);
            }
            if(mexeStock.comentario == 2) {
                maisStock manda;
                manda.num_products = mexeStock.num_products;
                manda.mtype = (long) mexeStock.dronetype;
                strcpy(manda.nome_prod, mexeStock.nome_prod);

                msgsnd(mq_id2, &manda, sizeof(manda) - sizeof(long), 0);
                //printf("Atualizado\n");
            }
        }
    }
}

//Movimentacao do drone conforme encomenda
void *controla_drone (void *arrayDrones) {
   time_t tempo = time(NULL);
   struct tm *t = localtime(&tempo);
   int hora, min, seg;
   float tTotal=0.0;
   Drones *drone = arrayDrones;
   signal(SIGINT, sinal_saida);

    while (1) {
        pthread_mutex_lock(&mutexes->get_queue);
        while (drone->estado == 1) {
            pthread_cond_wait(&cond_nao_escolhido, &(mutexes->get_queue));
            printf(" drone %d   estado: %d\n", drone->id, drone->estado);
        }

        pthread_mutex_unlock(&mutexes->get_queue);

        if (drone->estado == 2) {  //Drone a deslocar-se da base ate o armazem
            printf("DX: %0.2f   DY: %0.2f    AX:  %0.2f      AY:   %0.2f \n", drone->posI[0], drone->posI[1],
                   drone->encomenda.coordernadasArmazem[0], drone->encomenda.coordernadasArmazem[1]);
            printf("Drone com ID %d a movimentar se para o Armazem\n", drone->id);
            while (move_towards(&drone->posI[0], &drone->posI[1],drone->encomenda.coordernadasArmazem[0], drone->encomenda.coordernadasArmazem[1]) >= 0) {
                printf("DRONE %d a deslocar se para Armazem (X: %0.2f   Y: %0.2f)\n ",drone->id, drone->posI[0], drone->posI[1]);
                sleep(dados->unidadeT/5);
            }
            printf("Chegou no Armazem\n");

            drone->estado = 3;

        }

        if (drone->estado == 3) { //Drone a fazer o carregamento
            maisStock manda;
            manda.mtype = (long)drone->encomenda.idArmazem+1;
            manda.num_products = drone->encomenda.qtd;
            strcpy(manda.nome_prod,drone->encomenda.tipo_produto);
            manda.comentario = 2;
            manda.dronetype = drone->id;

            msgsnd(mq_id,&manda,sizeof(manda)-sizeof(long),0);

            maisStock recebe;
            sleep(manda.num_products);
            msgrcv(mq_id2, &recebe, sizeof(recebe) - sizeof(long),drone->id, 0);
            printf("Carregamento Aceite(Nome Prod: %s  Qt: %d) \n",recebe.nome_prod,recebe.num_products);

            pthread_mutex_lock(&mutexes->write_stats);
            estatisticas->prod_carregados += manda.num_products;
            pthread_mutex_unlock(&mutexes->write_stats);
            drone->estado = 4;
        }

        if (drone->estado == 4) {  //Drone a deslocar-se do armazem ate o destino
            printf("Drone com ID %d a movimentar se para o Destino\n", drone->id);
            while (move_towards(&drone->posI[0], &drone->posI[1],
                                drone->encomenda.coordenadas[0],
                                drone->encomenda.coordenadas[1]) >= 0) {
                printf("DRONE %d a deslocar se para o Destino (X: %0.2f   Y: %0.2f)\n ",drone->id, drone->posI[0], drone->posI[1]);
                sleep(dados->unidadeT/5);
            }
            drone->estado = 5;

            //calcula tempo de duracao da entrega da encomenda
            hora = (t->tm_hour) - (drone->encomenda.hora);
            min = (t->tm_min) - (drone->encomenda.min);
            seg = (t->tm_sec) - (drone->encomenda.seg);
            tTotal = (hora*3600)+(min*60)+seg;

            //atualiza estatisticas
            pthread_mutex_lock(&mutexes->write_stats);
            estatisticas->tempo_medio_individual += tTotal;
            estatisticas->encomendas_entregues += 1;
            estatisticas->prod_entregues += drone->encomenda.qtd;
            pthread_mutex_unlock(&mutexes->write_stats);
            
            //escreve no log
            sprintf(mensagem, "%d:%d:%d Encomenda %s-%d entregue no destino pelo drone %d\n",  t->tm_hour, t->tm_min, t->tm_sec, drone->encomenda.nomeEncomenda, drone->encomenda.nSque, drone->id);
            printf("%s", mensagem);
            pthread_mutex_lock(&mutexes->write_file);
            write_log(mensagem);
            pthread_mutex_unlock(&mutexes->write_file);
            mensagem[0] = '\0';

            //printf("Chegou ao Destino\n");
        }

        if (drone->estado == 5) {  //Drone a deslocar-se para uma base
            
            double array[2];
            escolherDestino(array,drone->posI[0],drone->posI[1]);

            printf("A Regressar a base\n");
            while (move_towards(&drone->posI[0], &drone->posI[1],array[0],array[1]) > 0) {
                printf("DRONE %d a deslocar se para Base (X: %0.2f   Y: %0.2f)\n",(drone->id), drone->posI[0], drone->posI[1]);
                sleep(dados->unidadeT/5);
            }

            printf("Chegou a base\n");
            drone->estado = 1;
        }
    }
}

//Escolhe base mais proxima de um drone
void escolherDestino(double *buf,double x, double y){
    for(int i =0; i < 4; i++) {
        if( i % 4 == 1) {
            //(double) 0;
            //(double)dados->max_y;

            if(distance(x,y,
                        (double) 0,(double)dados->max_y) < distance(x,y,buf[0],buf[1])){
                buf[0] = 0;
                buf[1] = dados->max_y;
            }

        } else if (i%4 == 2) {

            //(double)dados->max_x;
            //(double)dados->max_y;

            if(distance(x,y,
                        (double)dados->max_x,(double)dados->max_y) < distance(x,y,buf[0],buf[1])){
                buf[0] = dados->max_x;
                buf[1] = dados->max_y;
            }

        } else if (i%4 == 3){
            //(double) dados->max_x;
            //(double) 0;

            if(distance(x,y,
                        (double) dados->max_x,(double)0) < distance(x,y,buf[0],buf[1])){
                buf[0] = dados->max_x;
                buf[1] = 0;
            }

        } else if (i%4 == 0) {
            // (double) 0;
            // (double) 0;

            if(distance(x,y,
                        (double) 0,(double)dados->max_y) < distance(x,y,buf[0],buf[1])){
                buf[0] = 0;
                buf[1] = 0;
            }
        }
    }
}

//escolhe o armazem para uma encomenda
void escolhe_armazem(Encomenda *novoNode){
    int flag = 1;
    for(int k = 0;k < dados->numWh;k++) {
        for (int i = 0; i < 3; i++) {
            if (strcmp(armazensShm[k].produtos[i].produto,novoNode->tipo_produto) == 0) {
                if(armazensShm[k].produtos[i].qt >= novoNode->qtd){
                    novoNode->coordernadasArmazem[0] = armazensShm[k].coordenadas[0];
                    novoNode->coordernadasArmazem[1] = armazensShm[k].coordenadas[1];
                    novoNode->idArmazem = k;
                    pthread_mutex_lock(&mutexes->write_armazens);
                    armazensShm[k].produtos[i].qt =  armazensShm[k].produtos[i].qt - novoNode->qtd;
                    pthread_mutex_unlock(&mutexes->write_armazens);
                    novoNode->validade = 1;
                    flag = 0;
                    //printf("VALIDADE %d\n",novoNode->validade);
                    break;
                }
                else
                    novoNode->validade = 0; //nao ha produtos suficientes, adicionamos a fila de mensagem
            }
        }
        if(flag == 0)
            break;
    }
}

//escolhe o drone para uma encomenda
int escolheDrone(Encomenda *novoNode){
    printf("SELECIONAR O DRONE PARA A ENCOMENDA FEITA\n");
    int ID = -1;
    for(int i = 0; i < dados->n_drones; i++)
    {
        if(arrayDrones[i].estado == 1 || arrayDrones[i].estado == 5){
                ID = i;
                break;
        }
    }
    if(ID > -1){
        for(int i = 0; i < dados->n_drones; i++)
        {
            if(arrayDrones[i].estado == 1 || arrayDrones[i].estado == 5){
                if(distance(arrayDrones[ID].posI[0],arrayDrones[ID].posI[1],
                            novoNode->coordernadasArmazem[0],novoNode->coordernadasArmazem[1])
                 + distance(novoNode-> coordenadas[0], novoNode->coordenadas[1], 
                            novoNode->coordernadasArmazem[0],novoNode->coordernadasArmazem[1])
                < distance(arrayDrones[i].posI[0],arrayDrones[i].posI[1],
                            novoNode->coordernadasArmazem[0],novoNode->coordernadasArmazem[1])
                 + distance(novoNode-> coordenadas[0], novoNode->coordenadas[1], 
                            novoNode->coordernadasArmazem[0],novoNode->coordernadasArmazem[1]))
                {
                    ID = i;
                }
            }
        }

        arrayDrones[ID].estado=2;   //o drone ja nao esta mais em repouso 
        printf("Drone%d mudou para %d\n",arrayDrones[ID].id,arrayDrones[ID].estado);
        novoNode->id_drone = ID+1;    //guarda em encomenda o id do drone responsavel por ela

        //guarda as informacoes da encomenda no drone
        arrayDrones[ID].encomenda.tipo_produto = novoNode->tipo_produto;
        arrayDrones[ID].encomenda.coordernadasArmazem[0] = novoNode->coordernadasArmazem[0];
        arrayDrones[ID].encomenda.coordernadasArmazem[1] = novoNode->coordernadasArmazem[1];
        arrayDrones[ID].encomenda.idArmazem = novoNode->idArmazem;
        arrayDrones[ID].encomenda.coordenadas[0] = novoNode->coordenadas[0];
        arrayDrones[ID].encomenda.coordenadas[1] = novoNode->coordenadas[1];
        arrayDrones[ID].encomenda.nSque = novoNode->nSque;
        arrayDrones[ID].encomenda.nomeEncomenda = novoNode->nomeEncomenda;
        arrayDrones[ID].encomenda.qtd = novoNode ->qtd;
        arrayDrones[ID].encomenda.hora = novoNode ->hora;
        arrayDrones[ID].encomenda.min = novoNode ->min;
        arrayDrones[ID].encomenda.seg = novoNode ->seg;
        
        //atualiza estatisticas
        pthread_mutex_lock(&mutexes->write_stats);
        estatisticas->encomendas_atribuidas += 1;
        pthread_mutex_unlock(&mutexes->write_stats);

        return 1;
    } else {    //caso todos os drones estejam ocupados, adicionamos a fila
        return -1;
    }
}

//cria qtd drones
void criaDrones(int numI, int qtd){
    int i=0;
    signal(SIGINT, sinal_saida);
    //cria threads
    my_thread = malloc(dados->qtd * sizeof(pthread_t));
    arrayDrones = (Drones*)malloc(sizeof(Drones)*qtd);
    
    for(i=numI; i < qtd; i++) {
        if( i % 4 == 1) {
            arrayDrones[i].posI[0] = (double) 0;
            arrayDrones[i].posI[1] = (double)dados->max_y;
        } else if (i%4 == 2) {
            arrayDrones[i].posI[0] = (double)dados->max_x;
            arrayDrones[i].posI[1] = (double)dados->max_y;
        } else if (i%4 == 3) {
            arrayDrones[i].posI[0] = (double) dados->max_x;
            arrayDrones[i].posI[1] = (double) 0;
        } else if (i%4 == 0) {
            arrayDrones[i].posI[0] = (double) 0;
            arrayDrones[i].posI[1] = (double) 0;
        }
        arrayDrones[i].id = i+1;
        arrayDrones[i].estado = 1;
    }

    //long id[qtd+1];
    for(i=numI; i < qtd; i++) {
        //id[i] = i;
        printf("->Thread Drone%d criada\t\t", arrayDrones[i].id);
        pthread_create(&my_thread[i],NULL, controla_drone, &arrayDrones[i]);
        printf("Base x: %0.2f Base Y: %0.2f\n",arrayDrones[i].posI[0],arrayDrones[i].posI[1]);
    }
    sleep(5);
    printf("->Threads Criadas\n");
}

//gestao do pipe e dos drones
void central(){
    headListaE = (Encomenda *) malloc(sizeof(Encomenda));
    headListaE->next = NULL;
    Encomenda *novoNode = malloc(sizeof(Encomenda));
    time_t tempo = time(NULL);
    struct tm *t = localtime(&tempo);
    char *str = "Prod_";
    char nome_produto;
    int quantidade, posX, posY;
    char *token;

    //cria o pipe
    if ((mkfifo(PIPE_NAME, O_CREAT|O_EXCL|0600)<0) && (errno != EEXIST)){
        printf("Erro ao criar o PIPE\n");
    } else {
        printf("->Named Pipe criado.\n");
    }

    criaDrones(0, dados->n_drones);

    //abre o pipe

    while(1){
        int fd;
        int n_char;
        char buf[128], linha[128];
        char nome_ordem[20];

        if ((fd = open(PIPE_NAME, O_RDONLY)) < 0) {
            perror("CANNOT OPEN PIPE FOR READING");
            exit(0);
        }

        Encomenda *aux, *atual, *anterior;
        aux = headListaE;
        atual = headListaE->next;
        anterior = headListaE;
        n_char = read(fd, buf, 128);
        buf[n_char-1] = '\0';
        printf("%s",buf);
        fflush(stdout);
        strcpy(linha, buf);
        token = strtok(buf, " ");
        if (strcmp(token, "ORDER") == 0) {
            sscanf(linha, "ORDER %s Prod: %c, %d to: %d, %d", nome_ordem, &nome_produto, &quantidade, &posX, &posY);
            size_t len = strlen(str);
            char *produto = malloc(len + 1 + 1);
            strcpy(produto, str);
            produto[len] = nome_produto;
            produto[len + 1] = '\0';
            int i=0;
            while (strcmp(dados->tipos_produtos[i], produto) != 0) {
                i++;
            }
            /*printf("%d drones possiveis\n", dados->n_drones);
            for (int i=0; i<dados->n_drones; i++){
                printf("id = %d\n", arrayDrones[i].id);
            }*/
            printf("\nOpcao: ORDER\n");
            if (strcmp(dados->tipos_produtos[i], produto) == 0) {
                if(posX <= dados->max_x && posX >= 0 && posY <= dados->max_y && posY >= 0) {
                    //guardaa as informacoes do pipe no no
                    novoNode->nomeEncomenda = nome_ordem;
                    novoNode->qtd = quantidade;
                    novoNode->tipo_produto = produto;
                    novoNode->coordenadas[0] = (double) posX;
                    novoNode->coordenadas[1] = (double) posY;
                    novoNode->nSque = id_encomenda + 1;
                    id_encomenda++;

                    //guarda o horario que a encomenda foi criada
                    novoNode->hora = t->tm_hour;
                    novoNode->min = t->tm_min;
                    novoNode->seg = t->tm_sec;

                    sprintf(mensagem, "%d:%d:%d Encomenda %s-%d recebida pela Central\n", novoNode->hora, novoNode->min, novoNode->seg, novoNode->nomeEncomenda, novoNode->nSque);
                    printf("%s", mensagem);
                    pthread_mutex_lock(&mutexes->write_file);
                    write_log(mensagem);
                    pthread_mutex_unlock(&mutexes->write_file);
                    mensagem[0] = '\0';
                    
                    pthread_mutex_lock(&mutexes->retirar_mq);
                    escolhe_armazem(novoNode);
                    if (aux->next == NULL){ //nao ha mensagens na fila
                        if (novoNode->validade == 1) { //ha stock no armazem
                            if (escolheDrone(novoNode) > 0){
                                sprintf(mensagem, "%d:%d:%d Encomenda %s-%d enviada ao drone %d\n", t->tm_hour, t->tm_min, t->tm_sec, novoNode->nomeEncomenda, novoNode->nSque, novoNode->id_drone);
                                printf("%s", mensagem);
                                pthread_mutex_lock(&mutexes->write_file);
                                write_log(mensagem);
                                pthread_mutex_unlock(&mutexes->write_file);
                                mensagem[0] = '\0';
                                pthread_cond_broadcast(&cond_nao_escolhido);

                            } else if (escolheDrone(novoNode) < 0) { //todos os drones ocupados
                                aux->next = novoNode;
                            }
                        } else { //nao ha stock em nenhum armazem

                            sprintf(mensagem, "%d:%d:%d Encomenda %s-%d suspensa por falta de stock\n", t->tm_hour, t->tm_min, t->tm_sec, novoNode->nomeEncomenda, novoNode->nSque);
                            printf("%s", mensagem);
                            pthread_mutex_lock(&mutexes->write_file);
                            write_log(mensagem);
                            pthread_mutex_unlock(&mutexes->write_file);
                            mensagem[0] = '\0';
                            //adiciona a lista
                            aux->next = novoNode;

                        }
                    } else {// ha mensagens na fila

                        while(atual != NULL)
                        {
                            escolhe_armazem(atual);
                            if(atual->validade == 1 && escolheDrone(atual) > 0){

                                sprintf(mensagem, "%d:%d:%d Encomenda %s-%d enviada ao drone %d\n", t->tm_hour, t->tm_min, t->tm_sec, atual->nomeEncomenda, atual->nSque, atual->id_drone);
                                printf("%s", mensagem);
                                pthread_mutex_lock(&mutexes->write_file);
                                write_log(mensagem);
                                pthread_mutex_unlock(&mutexes->write_file);
                                mensagem[0] = '\0';
                                pthread_cond_broadcast(&cond_nao_escolhido);
                                //retira encomenda da fila
                                anterior->next = atual->next;
                                atual->next = NULL;
                                free(atual);
                                atual=anterior->next;
                            } else {
                                //continua a percorrer a fila
                                atual = atual->next;
                                anterior = anterior->next;
                            }
                        }

                        escolhe_armazem(novoNode);
                        if(novoNode->validade == 0 || escolheDrone(novoNode) < 0) { //nao ha stock ou todos os drones estao ocupados
                            //escrever no log
                            if(atual->validade == 0){
                                sprintf(mensagem, "%d:%d:%d Encomenda %s-%d suspensa por falta de stock\n", t->tm_hour, t->tm_min, t->tm_sec, novoNode->nomeEncomenda, novoNode->nSque);
                                printf("%s", mensagem);
                                pthread_mutex_lock(&mutexes->write_file);
                                write_log(mensagem);
                                pthread_mutex_unlock(&mutexes->write_file);
                                mensagem[0] = '\0';
                            }
                            //adicionar a fila
                            while (aux->next != NULL) {
                                aux = aux->next;
                            }
                            aux->next = novoNode;
                            novoNode->next = NULL;

                        } else if (novoNode->validade == 1 && escolheDrone(novoNode) > 0) {
                            //escrever no log
                            sprintf(mensagem, "%d:%d:%d Encomenda %s-%d enviada ao drone %d\n", t->tm_hour, t->tm_min, t->tm_sec, novoNode->nomeEncomenda, novoNode->nSque, novoNode->id_drone);
                            printf("%s", mensagem);
                            pthread_mutex_lock(&mutexes->write_file);
                            write_log(mensagem);
                            pthread_mutex_unlock(&mutexes->write_file);
                            mensagem[0] = '\0';
                            //broadcast
                            pthread_mutex_lock(&(mutexes->get_queue));
                            pthread_cond_broadcast(&cond_nao_escolhido);
                            pthread_mutex_unlock(&(mutexes->get_queue));
                        }
                    }
 
                    pthread_mutex_unlock(&mutexes->retirar_mq);

                } else {
                    sprintf(mensagem, "%d:%d:%d Coordenada invalida: %s \n", t->tm_hour, t->tm_min, t->tm_sec, linha);
                    pthread_mutex_lock(&mutexes->write_file);
                    write_log(mensagem);
                    pthread_mutex_unlock(&mutexes->write_file);
                }

            } else {
                sprintf(mensagem, "%d:%d:%d Produto invalido: %s \n", t->tm_hour, t->tm_min, t->tm_sec, linha);
                pthread_mutex_lock(&mutexes->write_file);
                write_log(mensagem);
                pthread_mutex_unlock(&mutexes->write_file);
            }
        }
        else if (strcmp(token, "DRONE") == 0) {
            int num;
            sscanf(linha, "DRONE SET %d", &num);
            if(num < dados->n_drones) {
                for(int k= num ; k < dados->n_drones; k++){
                    if (arrayDrones[k].estado == 1 || arrayDrones[k].estado == 5){
                        pthread_cancel(my_thread[k]);
                        pthread_join(my_thread[k], NULL);
                        arrayDrones[k].estado = 0;
                        arrayDrones[k].id = 0;
                        arrayDrones[k].posI[0] = 0;
                        arrayDrones[k].posI[1] = 0;
                        arrayDrones[k].posF[0] = 0;
                        arrayDrones[k].posF[1] = 0;
                    } else {
                        pthread_mutex_lock(&mutexes->drones);
                        pthread_cancel(my_thread[k]);
                        pthread_join(my_thread[k], NULL);
                        pthread_mutex_unlock(&mutexes->drones);
                        arrayDrones[k].estado = 0;
                        arrayDrones[k].id = 0;
                        arrayDrones[k].posI[0] = 0;
                        arrayDrones[k].posI[1] = 0;
                        arrayDrones[k].posF[0] = 0;
                        arrayDrones[k].posF[1] = 0;
                    }
                    printf("\nForam destruidas %d threads\n", dados->n_drones - num);
                    dados->n_drones = num;
                }
            } else if (num > dados->n_drones){
                    criaDrones(dados->n_drones,num);
                    dados->n_drones = num;
            } else {
                printf("\nJa estao derteminadas %d threads\n", num);
            }
        } else {
            printf("Comando invalido\nTente:\n\tORDER <order name> prod: <product name>, <quantity> to: <x>, <y>\n\tDRONE SET <num>\n");
        }
    }
}

//informacoes estatisticas
void sinal_estatistica(){
    //printf("entrei\n");
    //Calcula tempo medio
    if (estatisticas->tempo_medio_individual == 0) {
        estatisticas->tempo_medio_total = 0;
    } else {
        estatisticas->tempo_medio_total = (float) ((estatisticas->tempo_medio_individual)/(estatisticas->encomendas_entregues));
    }

    printf("\nInformação estatistica: \n");
    printf("Numero total de encomendas entregues = %d\n", estatisticas->encomendas_entregues);
    printf("Numero total de encomendas atribuidas = %d\n", estatisticas->encomendas_atribuidas);
    printf("Numero total de produtos carregados = %d\n", estatisticas->prod_carregados);
    printf("Numero total de produtos entregues = %d\n", estatisticas->prod_entregues);
    printf("Tempo medio = %0.2f\n", estatisticas->tempo_medio_total);    
}

//adiciona no ficheiro log a mensagem fornecida
void write_log(char* mensagem) { //este valor remete para o ID por exemplo do drone ou armazem
    FILE *fp = fopen("log.txt","a");
    if(fp != NULL){
        fseek(fp, 0, SEEK_END);
        fprintf(fp,"%s", mensagem);
        fclose(fp);
    }
    mensagem[0]='\0';
}

//encerra o programa
void sinal_saida (int sig){
    //CTRL + C
    //tempo
    time_t tempo = time(NULL);
    struct tm*t =localtime(&tempo);
    pthread_mutex_unlock(&mutexes->ctrlc);
    printf("a encerrar o programa.\n");
    sinal_estatistica();

    if(getpid() == processo_central){
        if(unlink(PIPE_NAME)==0){
            printf("\tpipe fechado\n");
        }
        for (int i = 0; i < dados->n_drones; i++) {
            if (pthread_cancel(my_thread[i]) != 0) {
                perror("Error canceling thread");
                exit(1);
            }
            if (pthread_join(my_thread[i], NULL) != 0) {
                perror("Error joining thread");
                exit(1);
            }
            free(my_thread);
        }
        printf("\tcentral terminada\n" );
        sprintf(mensagem, "%d:%d:%d Fim do programa\n",t->tm_hour,t->tm_min,t->tm_sec);
        printf("%s", mensagem);
        pthread_mutex_lock(&mutexes->write_file);
        write_log(mensagem);
        pthread_mutex_unlock(&mutexes->write_file);

        //matar processos
        kill(processo_central, SIGKILL);

    }else if(getpid()==processo_gestor){

        if(unlink(PIPE_NAME)==0){
            printf("\tpipe fechado\n");
        }

        //liberar memoria partilhada
        shmdt(&estatisticas);
        shmctl(shmid_estats, IPC_RMID, NULL);
        shmdt(&armazens);
        shmctl(shmid_armazens, IPC_RMID, NULL);
        printf("\tmemoria partilhada liberada.\n");

        // Message Queue
        msgctl(mq_id, IPC_RMID, NULL);
        printf("\tmessage queue destruida\n");
        msgctl(mq_id2, IPC_RMID, NULL);
        printf("\tmessage queue destruida\n");

        //semaforos
        /*if(sem_shmid>=0){
            semctl(sem_shmid, 0, IPC_RMID);
        }*/
        printf("\tsemaforos destruidos.\n");

        //liberar mallocs
        free(dados);
        printf("\tmallocs libertados.\n");


        sprintf(mensagem, "%d:%d:%d Fim do programa\n",t->tm_hour,t->tm_min,t->tm_sec);
        printf("%s", mensagem);
        pthread_mutex_lock(&mutexes->write_file);
        write_log(mensagem);
        pthread_mutex_unlock(&mutexes->write_file);

        //mutex
        if(mutex_shmid>=0){
            semctl(mutex_shmid, 0, IPC_RMID);
        }
        pthread_mutex_destroy(&mutexes->write_file);
        pthread_mutex_destroy(&mutexes->ctrlc);
        pthread_mutex_destroy(&mutexes->get_queue);
        pthread_mutex_destroy(&mutexes->retirar_mq);
        pthread_mutex_destroy(&mutexes->write_stats);
        pthread_mutex_destroy(&mutexes->write_armazens);
        pthread_mutex_destroy(&mutexes->drones);

        //cond
        pthread_cond_destroy(&cond_nao_escolhido);

        printf("\tmutexes destruidos.\n");

        //matar processos
        kill(processo_gestor, SIGKILL);

    } else {    //processo armazem

        for(int i=0; i<dados->numWh; i++){
            sprintf(mensagem, "%d:%d:%d Fim do processo armazem %d\n",t->tm_hour,t->tm_min,t->tm_sec,getpid());
            printf("%s", mensagem);
            pthread_mutex_lock(&mutexes->write_file);
            write_log(mensagem);
            pthread_mutex_unlock(&mutexes->write_file);
            kill(armazensShm[i].pid, SIGKILL);
        }
        sprintf(mensagem, "%d:%d:%d Fim do programa\n",t->tm_hour,t->tm_min,t->tm_sec);
        printf("%s", mensagem);
        pthread_mutex_lock(&mutexes->write_file);
        write_log(mensagem);
        pthread_mutex_unlock(&mutexes->write_file);
    }
}