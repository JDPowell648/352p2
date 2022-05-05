#include <stdio.h>
#include <stdint.h>
#include "encrypt-module.h"
#include <stdlib.h>
#include <pthread.h>
#include <stdbool.h>
/**
 * This file handles the encryption of a file. Using five different threads, we:
 * 1. Read from the file and insert into a queue
 * 2. Count every new item added
 * 3. Encrypt the characters and send them to an output queue
 * 4. Count every new item added to the output queue
 * 5. Write to the output file
 * All of these are done in their own thread which run concurrently. Sometimes a reset may be called, in which case we clear the queues out and then log the  * counts of the input and output queues, and then allow the reset to happen.
 *
 **/

//Struct for objects in the queue
typedef struct {
	char c;
	int counted;
} qObj;

//Struct for the queue which implements FIFO
typedef struct {
	int maxSize;
	int currentSize;
	qObj ** queue;
	int head;
	int tail;
} FIFOqueue;

//Initializes a queue object with a character c
qObj* initObj(unsigned char c){
        qObj* new = malloc(sizeof(qObj));
        new->counted = 0;
	new->c = c;
	return new;
}

//Initilizes a queue with a specified size
void initQueue(int size, FIFOqueue *toInit){
	toInit->queue = malloc(sizeof(qObj) * size);
	for(int i = 0; i < size; i++){
		qObj* newObj = initObj('0');
		toInit->queue[i] = newObj;
	}
	toInit->maxSize = size;
	toInit->currentSize = 0;
	toInit->tail = -1;
	toInit->head = 0;	
}

//Adds a new object to the end of the queue
int enqueue(FIFOqueue* q, char data){
	if(q->currentSize !=  q->maxSize){
		q->queue[q->tail + 1]->c = data;
		q->queue[q->tail + 1]->counted = 0;
		q->currentSize++;
		q->tail++;
		return 1;
	}else{
		printf("FAILED TO ENQUEUE");
		return -1; //failed to enqueue
	}
}

//Removes an object from the front of the queue and shifts everything else down
char dequeue(FIFOqueue* q){
	char c = q->queue[0]->c;
	if(q->tail > 0){
		for(int i = 0; i < q->tail; i++){
			q->queue[i]->c = q->queue[i+1]->c;
			q->queue[i]->counted = q->queue[i+1]->counted;
		}
		q->queue[q->tail]->counted = 0;
		q->tail--;
		q->currentSize--;
	}else if(q->tail == 0){
		q->tail = -1;
		q->currentSize = 0;
		q->queue[0]->counted = 0;
	}
	return c;
}

//Prints out the values in the queue
void printQueue(FIFOqueue* q){
	for(int i = 0; i <= q->tail; i++){
		printf("%c, ", q->queue[i]->c);
	}
	printf("\n");
}

FIFOqueue *inputBuffer;
FIFOqueue *outputBuffer;
pthread_t reader_thread, input_thread, encryption_thread, output_thread, writer_thread;
pthread_cond_t read_wait, encrypt_wait, read_wait_encrypt, output_wait, input_wait, oq_wait, write_encrypt_wait, writer_wait, reset_read_wait, reset_wait; 
pthread_mutex_t mute;
int reset_request = 0;

//This is called when the encryption module requests a reset, in which case we need to clean out the queues
void reset_requested() {
	reset_request = 1;
	while(inputBuffer->currentSize > 0 || outputBuffer->currentSize > 0);
	log_counts();
}

//Lets the module know that its okay to reset and that the reader thread can start back up
void reset_finished() {
	reset_request = 0;
	pthread_cond_signal(&reset_read_wait);	
}

//Thread for reading from the input file. Gets blocked if the input queue is full
void *readerThread(void *arg) {
	char c;
	while((c = read_input()) != EOF) {
                if(reset_request == 1){
			pthread_mutex_lock(&mute);
                        pthread_cond_wait(&reset_read_wait, &mute);
			pthread_mutex_unlock(&mute);
                }
		if(inputBuffer->currentSize == inputBuffer->maxSize) {
			pthread_mutex_lock(&mute);
			pthread_cond_wait(&encrypt_wait, &mute);
			pthread_mutex_unlock(&mute);
		}
		int result = enqueue(inputBuffer, c);
		if(inputBuffer->currentSize != 0) {
			pthread_cond_signal(&read_wait);
			pthread_cond_signal(&read_wait_encrypt);
		}
	}
	printf("End of file\n");
	while(inputBuffer->currentSize == inputBuffer->maxSize);
	enqueue(inputBuffer, EOF);
	pthread_cond_signal(&read_wait);
	pthread_cond_signal(&read_wait_encrypt);
	log_counts();
	pthread_exit(NULL);
}

//Thread for counting the input characters. Gets blocked if the input queue is empty
void *inputCounterThread(void *arg){
	//printf("inputCounterThread started\n");
	while(1) {
		if(inputBuffer->currentSize == 0) {
			pthread_mutex_lock(&mute);
			pthread_cond_wait(&read_wait, &mute);
			pthread_mutex_unlock(&mute);
		}
		if(inputBuffer->queue[0]->c == EOF){
			break;
		}
		if(inputBuffer->queue[0]->counted == 0 && inputBuffer->currentSize > 0) {
			count_input(inputBuffer->queue[0]->c);
			inputBuffer->queue[0]->counted = 1;
		}
		if(inputBuffer->queue[0]->counted == 1) {
			pthread_cond_signal(&input_wait);
		}
	}
	pthread_cond_signal(&input_wait);
	inputBuffer->queue[0]->counted = 1;
	pthread_exit(NULL);
}

//Thread for encrypting the characters and sending them to the output counter. Gets blocked if the input queue is empty or the output queue is full.
void *encryptionThread(void *arg) {
	//printf("encryptionThread started\n");
	while(1) {
		if(inputBuffer->currentSize == 0) {
			pthread_mutex_lock(&mute);
			pthread_cond_wait(&read_wait_encrypt, &mute);
			pthread_mutex_unlock(&mute);
		}
		if(inputBuffer->queue[0]->counted == 0) {
			pthread_mutex_lock(&mute);
			pthread_cond_wait(&input_wait, &mute);
			pthread_mutex_unlock(&mute);
		}
		if(outputBuffer->currentSize == outputBuffer->maxSize) {
                        pthread_mutex_lock(&mute);
                        pthread_cond_wait(&writer_wait, &mute);
                        pthread_mutex_unlock(&mute);
                }
		char c = dequeue(inputBuffer);
		if(c != EOF){
			char newC = encrypt(c);
			enqueue(outputBuffer, newC);
		}else{
			break;
		}
		if(inputBuffer->currentSize < inputBuffer->maxSize){
			pthread_cond_signal(&encrypt_wait);
		}
		if(outputBuffer->currentSize > 0){
                        pthread_cond_signal(&write_encrypt_wait);
                	pthread_cond_signal(&oq_wait);
                }
	}
	pthread_cond_signal(&write_encrypt_wait);
	pthread_cond_signal(&encrypt_wait);
	pthread_cond_signal(&oq_wait);
	enqueue(outputBuffer, EOF);
	pthread_exit(NULL); 
}

//Thread for counting the characters in the output queue. Blocks if the output queue is empty.
void *outputCounterThread(void *arg) {
	//printf("outputCounterThread started\n");
	while(1){
		if(outputBuffer->currentSize == 0) {
                        pthread_mutex_lock(&mute);
                        pthread_cond_wait(&oq_wait, &mute);
                        pthread_mutex_unlock(&mute);
                }
		if(outputBuffer->queue[0]->c == EOF){
			pthread_cond_signal(&output_wait);
			break;
		}
		if(outputBuffer->queue[0]->counted == 0 && outputBuffer->currentSize > 0) {
			count_output(outputBuffer->queue[0]->c);
			outputBuffer->queue[0]->counted = 1;
		}
		if(outputBuffer->queue[0]->counted == 1) {
			pthread_cond_signal(&output_wait);
		}
	}
	pthread_cond_signal(&output_wait);
	pthread_exit(NULL);
}

//Thread for writing to the output file. Blocks if the output queue is empty.
void *writerThread(void *arg) {
	//printf("writerThread started\n");
	while(1){
		if(outputBuffer->currentSize == 0){
			pthread_mutex_lock(&mute);
                        pthread_cond_wait(&write_encrypt_wait, &mute);
                        pthread_mutex_unlock(&mute);
		}
                if(outputBuffer->queue[0]->counted == 0) {
                        pthread_mutex_lock(&mute);
                        pthread_cond_wait(&output_wait, &mute);
                        pthread_mutex_unlock(&mute);
                }
		char c = dequeue(outputBuffer);
		if(c == EOF){
                        break;
                }
                write_output(c);
		if(outputBuffer->currentSize < outputBuffer->maxSize) {
			pthread_cond_signal(&writer_wait);
		}
	}
	pthread_cond_signal(&writer_wait);
	pthread_exit(NULL); 
}

//Creates the threads and initializes the global variables
int main(int argc, char *argv[]) {
	if ( argc != 4 ){
		printf("Not enough command line arguments\n");
		exit(0);
	}

	init(argv[1], argv[2], argv[3]);
	
	int N = -1;
	int M = -1;
	printf("Please input size for N > 1: ");
	scanf("%d", &N);
	while(N <= 1){
		printf("N must be greater than 1, input size for N > 1: ");
		scanf("%d", &N);
	}

	printf("Please input size for M > 1: ");
        scanf("%d", &M);
        while(M <= 1){
                printf("M must be greater than 1, input size for M > 1: ");
                scanf("%d", &M);
        }
	inputBuffer = malloc(sizeof(FIFOqueue));
	initQueue(N, inputBuffer);

	outputBuffer = malloc(sizeof(FIFOqueue));
	initQueue(M, outputBuffer);

	pthread_create(&reader_thread, NULL, readerThread, NULL);

	pthread_create(&input_thread, NULL, inputCounterThread, NULL);

	pthread_create(&encryption_thread, NULL, encryptionThread, NULL);

	pthread_create(&output_thread, NULL, outputCounterThread, NULL);

	pthread_create(&writer_thread, NULL, writerThread, NULL);
	
	pthread_join(reader_thread, NULL);
        printf("reader thread joined to main\n");
	pthread_join(input_thread, NULL); 
	printf("\ninput thread joined to main\n");
	pthread_join(encryption_thread, NULL);
	printf("encrypt thread joined to main\n");
	pthread_join(output_thread, NULL);
	printf("output thread joined to main\n");
	//pthread_cond_signal(&write_encrypt_wait);
	//pthread_cond_signal(&output_wait);
	pthread_join(writer_thread, NULL);
	printf("writer thread joined to main\n");

	printf("main exiting\n");
	exit(0);

}




