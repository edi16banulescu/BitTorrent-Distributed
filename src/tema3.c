#include <mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100

typedef struct {
    char hash[HASH_SIZE + 1];
} chunk;

typedef struct {
    char filename[MAX_FILENAME];
    int num_chunks;
    chunk chunks[MAX_CHUNKS];
} file_info;

typedef struct {
    int peer_rank;
    file_info file;
} peer_info;

typedef struct {
    int peer_ranks_no;
    char filename[MAX_FILENAME];
    peer_info peers[10];
} swarm_file;

void *download_thread_func(void *arg)
{
    int rank = *(int*) arg;
    char filename[MAX_FILENAME];
    int num_files;
    int wanted_files_no;
    file_info files[MAX_FILES];
    swarm_file swarm_files[MAX_FILES];
    char file_out[40];

    sprintf(filename, "in%d.txt", rank);

    FILE *f = fopen(filename, "r");

    if (f == NULL) {
        printf("Eroare la deschiderea fisierului %s\n", filename);
        exit(-1);
    }

    fscanf(f, "%d", &num_files);

    // SKIP UNTILL WANTED FILES
    for (int i = 0; i < num_files; i++) {
        fscanf(f, "%*s");
        int num_chunks;
        fscanf(f, "%d", &num_chunks);
        for (int j = 0; j < num_chunks; j++) {
            fscanf(f, "%*s");
        }
    }

    // GET WANTED FILES
    fscanf(f, "%d", &wanted_files_no);

    for (int i = 0; i < wanted_files_no; i++) {
        fscanf(f, "%s", files[i].filename);
    }

    fclose(f);

    // Send wanted files to tracker
    MPI_Send(&files, sizeof(files), MPI_BYTE, TRACKER_RANK, 0, MPI_COMM_WORLD);

    // Receive swarm files from tracker
    for (int i = 0; i < wanted_files_no; i++) {
        MPI_Recv(&swarm_files[i], sizeof(swarm_file), MPI_BYTE, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    }

    for (int i = 0; i < wanted_files_no; i++) {
        sprintf(file_out, "client%d_%s", rank, swarm_files[i].filename);

        FILE *f = fopen(file_out, "w");

        // Choose random peer
        int number_of_chunks = 0;
        int random_peer = rand() % swarm_files[i].peer_ranks_no;
        peer_info peer = swarm_files[i].peers[random_peer];
        int k = 0;
        MPI_Status status;
        file_info file_downloaded;
        strcpy(file_downloaded.filename, peer.file.filename);

        while (number_of_chunks < peer.file.num_chunks) {
            bool sent = false;
            file_downloaded.num_chunks = number_of_chunks + 1;
            strcpy(file_downloaded.chunks[number_of_chunks].hash, peer.file.chunks[number_of_chunks].hash);

            // Tell the tracker that we downloaded 10 chunks
            if (k % 10 == 0) {
                MPI_Send(&file_downloaded, sizeof(file_info), MPI_BYTE, TRACKER_RANK, 0, MPI_COMM_WORLD);
                sent = true;

                // Send wanted files to tracker to get updated swarm files
                MPI_Send(&files, sizeof(files), MPI_BYTE, TRACKER_RANK, 1000, MPI_COMM_WORLD);

                for (int j = 0; j < wanted_files_no; j++) {
                    MPI_Recv(&swarm_files[j], sizeof(swarm_file), MPI_BYTE, TRACKER_RANK, 1000, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                }
            }

            bool ack = false;

            // Send chunk request
            MPI_Send(&peer.file.chunks[number_of_chunks], sizeof(chunk), MPI_BYTE, peer.peer_rank, 0, MPI_COMM_WORLD);

            // Receive ack from peer
            MPI_Recv(&ack, sizeof(bool), MPI_BYTE, peer.peer_rank, peer.peer_rank, MPI_COMM_WORLD, &status);
            
            if (!ack) {
                printf("Eroare la primirea chunk-ului %s de la peer %d\n", peer.file.chunks[number_of_chunks].hash, peer.peer_rank);
                exit(-1);
            }

            number_of_chunks++;
            k++;

            // Choose another random peer so that the network does not get congested
            random_peer = rand() % swarm_files[i].peer_ranks_no;
            peer = swarm_files[i].peers[random_peer];

            if (number_of_chunks == peer.file.num_chunks) {
                // If we received all the chunks from the peer, but it isn't multiple of 10
                if (!sent) {
                    MPI_Send(&file_downloaded, sizeof(file_info), MPI_BYTE, TRACKER_RANK, 0, MPI_COMM_WORLD);

                    // Send wanted files to tracker to get updated swarm files
                    MPI_Send(&files, sizeof(files), MPI_BYTE, TRACKER_RANK, 1000, MPI_COMM_WORLD);

                    for (int j = 0; j < wanted_files_no; j++) {
                        MPI_Recv(&swarm_files[j], sizeof(swarm_file), MPI_BYTE, TRACKER_RANK, 1000, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                    }
                }
            }

        }

        // After we receive all the acks successfully, we can write the file
        for (int j = 0; j < peer.file.num_chunks; j++) {
            fprintf(f, "%s\n", peer.file.chunks[j].hash);
        }

        fclose(f);
    }

    // Tell tracker that we finished downloading
    bool finished = true;
    MPI_Send(&finished, sizeof(bool), MPI_BYTE, TRACKER_RANK, rank, MPI_COMM_WORLD);

    return NULL;
}

void *upload_thread_func(void *arg)
{
    int rank = *(int*) arg;
    MPI_Status status;
    bool isReceiving = true;
    sleep(1);

    // Receive chunk requests from peers
    while (isReceiving) {
        chunk chunk_recv;
        bool ack = false;

        // Receive from any source
        MPI_Recv(&chunk_recv, sizeof(chunk), MPI_BYTE, MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &status);

        // Check if we received a trash chunk from tracker as a stop signal
        if (status.MPI_SOURCE == TRACKER_RANK) {
            isReceiving = false;
            break;
        }

        ack = true;

        // Send ack to peer
        MPI_Send(&ack, sizeof(bool), MPI_BYTE, status.MPI_SOURCE, rank, MPI_COMM_WORLD);
    }

    return NULL;
}

void tracker(int numtasks, int rank) {
    swarm_file swarm_files[MAX_FILES];
    int swarm_files_no = 0;
    bool ack = 0;

    for (int i = 1; i < numtasks; i++) {
        // Receive files from peers and create swarm files
        file_info files_recv[MAX_FILES];
        MPI_Recv(&files_recv, sizeof(files_recv), MPI_BYTE, i, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        for (int j = 0; j < MAX_FILES; j++) {
            if (files_recv[j].filename[0] == '\0') {
                break;
            }
            // Parse files and create swarm files
            for (int k = 0; k < MAX_FILES; k++) {
                if (swarm_files[k].filename[0] == '\0') {
                    strcpy(swarm_files[k].filename, files_recv[j].filename);
                    swarm_files[k].peer_ranks_no = 1;
                    swarm_files[k].peers[0].peer_rank = i;
                    strcpy(swarm_files[k].peers[0].file.filename, files_recv[j].filename);
                    swarm_files[k].peers[0].file.num_chunks = files_recv[j].num_chunks;
                    for (int l = 0; l < files_recv[j].num_chunks; l++) {
                        strcpy(swarm_files[k].peers[0].file.chunks[l].hash, files_recv[j].chunks[l].hash);
                    }
                    swarm_files_no++;
                    break;
                } else if (strcmp(swarm_files[k].filename, files_recv[j].filename) == 0) {
                    swarm_files[k].peer_ranks_no++;
                    swarm_files[k].peers[swarm_files[k].peer_ranks_no - 1].peer_rank = i;
                    strcpy(swarm_files[k].peers[swarm_files[k].peer_ranks_no - 1].file.filename, files_recv[j].filename);
                    swarm_files[k].peers[swarm_files[k].peer_ranks_no - 1].file.num_chunks = files_recv[j].num_chunks;
                    for (int l = 0; l < files_recv[j].num_chunks; l++) {
                        strcpy(swarm_files[k].peers[swarm_files[k].peer_ranks_no - 1].file.chunks[l].hash, files_recv[j].chunks[l].hash);
                    }
                    break;
                }
            }
        }
    }

    // After all peers have registered their files, send signal to start downloading and uploading
    ack = 1;
    for (int i = 1; i < numtasks; i++) {
        MPI_Send(&ack, sizeof(bool), MPI_BYTE, i, 0, MPI_COMM_WORLD);
    }

    int total_wanted_files = 0;

    for (int i = 1; i < numtasks; i++) {
        // Receive wanted files from peers
        file_info files_recv[MAX_FILES];
        MPI_Recv(&files_recv, sizeof(files_recv), MPI_BYTE, i, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        for (int j = 0; j < MAX_FILES; j++) {
            if (files_recv[j].filename[0] == '\0') {
                break;
            }
            for (int k = 0; k < MAX_FILES; k++) {
                if (strcmp(swarm_files[k].filename, files_recv[j].filename) == 0) {
                    // Send wanted swarm files to peers
                    MPI_Send(&swarm_files[k], sizeof(swarm_file), MPI_BYTE, i, 0, MPI_COMM_WORLD);
                    // Count total wanted files
                    total_wanted_files++;    
                    break;
                }
                if (swarm_files[k].filename[0] == '\0') {
                    break;
                }
            }
        }
    }

    bool isReceiving = true;
    int file_downloaded_no = 0;

    while (isReceiving) {
        // Receive file info from peers
        MPI_Status status;
        file_info file_downloaded;
        MPI_Recv(&file_downloaded, sizeof(file_info), MPI_BYTE, MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &status);
        int peer_rank = status.MPI_SOURCE;
        file_info files_recv[MAX_FILES];

        // Update swarm files
        for (int i = 0; i < MAX_FILES; i++) {
            if (strcmp(swarm_files[i].filename, file_downloaded.filename) == 0) {
                for (int j = 0; j < 10; j++) {
                    peer_info current_peer = swarm_files[i].peers[j];
                    if (current_peer.peer_rank == peer_rank) {
                        current_peer.file.num_chunks = file_downloaded.num_chunks;
                        for (int k = 0; k < file_downloaded.num_chunks; k++) {
                            strcpy(current_peer.file.chunks[k].hash, file_downloaded.chunks[k].hash);
                        }
                        break;
                    } else if (current_peer.peer_rank == 0) {
                        current_peer.peer_rank = peer_rank;
                        strcpy(current_peer.file.filename, file_downloaded.filename);
                        current_peer.file.num_chunks = file_downloaded.num_chunks;
                        for (int k = 0; k < file_downloaded.num_chunks; k++) {
                            strcpy(current_peer.file.chunks[k].hash, file_downloaded.chunks[k].hash);
                        }
                        break;
                    }
                }

                // Check if peer has downloaded all the chunks
                if (swarm_files[i].peers[0].file.num_chunks == file_downloaded.num_chunks) {
                    file_downloaded_no++;
                }
            }
        }

        // We receive the wanted files again from peers to send the the updated swarm files
        MPI_Status status2;
        MPI_Recv(&files_recv, sizeof(files_recv), MPI_BYTE, MPI_ANY_SOURCE, 1000, MPI_COMM_WORLD, &status2);
        int peer2_rank = status2.MPI_SOURCE;
        for (int j = 0; j < MAX_FILES; j++) {
            if (files_recv[j].filename[0] == '\0') {
                break;
            }
            for (int k = 0; k < MAX_FILES; k++) {
                if (strcmp(swarm_files[k].filename, files_recv[j].filename) == 0) {
                    // Send wanted swarm files to peers
                    MPI_Send(&swarm_files[k], sizeof(swarm_file), MPI_BYTE, peer2_rank, 1000, MPI_COMM_WORLD);
                }
            }
        }

        // Stop if every peer has downloaded their wanted files
        if (file_downloaded_no == total_wanted_files) {
            isReceiving = false;
            break;
        }
    }

    bool finished = false;
    int finished_no = 0;
    // Receive finished downloading signal from peers
    for (int i = 1; i < numtasks; i++) {
        MPI_Recv(&finished, sizeof(bool), MPI_BYTE, i, i, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        if (finished) {
            finished_no++;
            finished = false;
        }
    }

    // If all peers finished downloading, send stop signal to peers so they can stop receiving chunks
    if (finished_no == numtasks - 1) {
        for (int i = 1; i < numtasks; i++) {
            chunk trash_chunk;
            MPI_Send(&trash_chunk, sizeof(chunk), MPI_BYTE, i, 0, MPI_COMM_WORLD);
        }
    }
}

void peer(int numtasks, int rank) {
    pthread_t download_thread;
    pthread_t upload_thread;
    void *status;
    int r;
    char filename[MAX_FILENAME];
    int num_files;
    file_info files[MAX_FILES];
    bool ack = 0;

    sprintf(filename, "in%d.txt", rank);

    FILE *f = fopen(filename, "r");

    if (f == NULL) {
        printf("Eroare la deschiderea fisierului %s\n", filename);
        exit(-1);
    }

    // Scan the files from the input file
    fscanf(f, "%d", &num_files);

    for (int i = 0; i < num_files; i++) {
        fscanf(f, "%s", files[i].filename);
        fscanf(f, "%d", &files[i].num_chunks);
        for (int j = 0; j < files[i].num_chunks; j++) {
            fscanf(f, "%s", files[i].chunks[j].hash);
        }
    }

    // Send files to tracker to register swarm
    MPI_Send(&files, sizeof(files), MPI_BYTE, TRACKER_RANK, 0, MPI_COMM_WORLD);

    // Receive ack from tracker so we can start downloading and uploading
    MPI_Recv(&ack, sizeof(bool), MPI_BYTE, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);


    if (ack == 0) {
        printf("Eroare la primirea confirmarii de la tracker\n");
        exit(-1);
    }

    fclose(f);

    r = pthread_create(&download_thread, NULL, download_thread_func, (void *) &rank);
    if (r) {
        printf("Eroare la crearea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_create(&upload_thread, NULL, upload_thread_func, (void *) &rank);
    if (r) {
        printf("Eroare la crearea thread-ului de upload\n");
        exit(-1);
    }

    r = pthread_join(download_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_join(upload_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de upload\n");
        exit(-1);
    }
}
 
int main (int argc, char *argv[]) {
    int numtasks, rank;
 
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE) {
        fprintf(stderr, "MPI nu are suport pentru multi-threading\n");
        exit(-1);
    }
    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == TRACKER_RANK) {
        tracker(numtasks, rank);
    } else {
        peer(numtasks, rank);
    }

    MPI_Finalize();
}
