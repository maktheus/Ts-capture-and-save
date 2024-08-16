#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/time.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include "player.h"
#include "channel_parser.h"
#include "tuner.h"
#include "demux.h"

#define FRONTEND_PATH "/dev/dvb/adapter0/frontend0"
#define DEMUX_PATH "/dev/dvb/adapter0/demux0"
#define DVR_PATH "/dev/dvb/adapter0/dvr0"

#define BUFFER_SIZE 4096
#define WAIT_TIME 5000 // Milissegundos
// 1 hora
#define SWITCH_INTERVAL 1800000

#define SAVE_DIR "tsSaveBackup"

// Função para criar o diretório de backup se ele não existir
void create_save_dir_if_needed() {
    struct stat st = {0};
    if (stat(SAVE_DIR, &st) == -1) {
        if (mkdir(SAVE_DIR, 0700) == -1) {
            perror("Error creating save directory");
            exit(EXIT_FAILURE);
        }
    }
}

static double currentTimeMillis() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

void create_timestamped_filename(char *buffer, size_t len, const char *extension) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    // Formata a data e hora e cria o nome do arquivo com extensão
    strftime(buffer, len, SAVE_DIR "/output_%Y%m%d_%H%M%S", t);
    snprintf(buffer + strlen(buffer), len - strlen(buffer), ".%s", extension);
}

void upload_to_drive(const char *local_filename, const char *remote_path) {
    char command[512];
    snprintf(command, sizeof(command), "rclone copy %s tsSaver:%s/ -v --progress --stats 1s --transfers=4 --checksum", local_filename, remote_path);
    system(command);
}

void compress_file(const char *filename) {
    char command[256];
    snprintf(command, sizeof(command), "gzip %s", filename);
    system(command);
}

void delete_file(const char *filename) {
    if (remove(filename) != 0) {
        perror("Error deleting file");
    }
}

void clear_directory(const char *path) {
    struct dirent *entry;
    DIR *dir = opendir(path);

    if (dir == NULL) {
        perror("Error opening directory");
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            char file_path[512];
            snprintf(file_path, sizeof(file_path), "%s/%s", path, entry->d_name);
            delete_file(file_path);
        }
    }

    closedir(dir);
}

void convert_ts_to_aac(const char *input_filename, const char *output_filename) {
    char command[512];
    snprintf(command, sizeof(command), "ffmpeg -i %s -vn -c:a aac -b:a 192k %s", input_filename, output_filename);
    system(command);
}

int main(int argc, char *argv[]) {
    gst_init(NULL, NULL);

    // Criar o diretório de backup se necessário
    create_save_dir_if_needed();

    // Limpar o diretório de backup no início da execução
    clear_directory(SAVE_DIR);

    char *channel_file;
    if (argc > 2) {
        printf("Usage: main channel_file\n");
        return -1;
    } else if (argc == 2) {
        channel_file = argv[1];
    } else {
        channel_file = "dvb_channel.conf";
    }

    struct ChannelList channel_list = parse_channels(channel_file);
    struct ChannelListNode *current_node = channel_list.head;
    printf("Current Channel: %s\n", current_node->data.name);

    int frontend;
    if ((frontend = open(FRONTEND_PATH, O_RDWR | O_NONBLOCK)) < 0) {
        perror("FRONTEND OPEN: ");
        return -1;
    }

    if (tune(frontend, current_node->data.frequency) < 0) {
        perror("Não foi possível sintonizar o canal.");
        return -1;
    }

    int demux;
    if ((demux = open(DEMUX_PATH, O_RDWR | O_NONBLOCK)) < 0) {
        perror("Couldn't open demux: ");
        return -1;
    }

    if (setup_demux(demux) < 0) {
        perror("Não foi possível configurar demux");
        return -1;
    }

    double tuneClock = currentTimeMillis();
    while ((currentTimeMillis() - tuneClock) < WAIT_TIME) {
    }

    int dvr;
    if ((dvr = open(DVR_PATH, O_RDONLY)) < 0) {
        perror("DVR DEVICE: ");
        return -1;
    }

    player_t player;
    if (PlayerInit(&player) < 0) {
        printf("Player Init Failed\n");
        return -1;
    }

    PlayerStart(&player);

    char ts_filename[256]; // Aumentado para evitar truncamento
    create_timestamped_filename(ts_filename, sizeof(ts_filename), "ts");

    // Abrir o arquivo para salvar os dados recebidos
    FILE *output_file = fopen(ts_filename, "wb");
    if (!output_file) {
        perror("Could not open output file");
        return -1;
    }

    struct termios termios_original;
    tcgetattr(STDIN_FILENO, &termios_original);
    struct termios termios_modified = termios_original;
    termios_modified.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &termios_modified);

    fd_set fds;
    struct timeval tv;
    int stdin_fd = STDIN_FILENO;

    double start_time = currentTimeMillis();

    while (1) {
        FD_ZERO(&fds);
        FD_SET(stdin_fd, &fds);

        tv.tv_sec = 0;
        tv.tv_usec = 0;

        int ready = select(stdin_fd + 1, &fds, NULL, NULL, &tv);

        if (ready == -1) {
            perror("select");
            break;
        } else if (ready > 0) {
            if (FD_ISSET(stdin_fd, &fds)) {
                char input;
                ssize_t bytes = read(stdin_fd, &input, 1);
                if (bytes == 1) {
                    // Handle user input
                    if (input == 'q') {
                        printf("Exiting...\n");
                        break;
                    }
                }
            }
        }

        // Trocar de canal a cada 1 minuto
        if ((currentTimeMillis() - start_time) >= SWITCH_INTERVAL) {
            printf("Switching channels...\n");

            PlayerFree(&player);
            close(dvr);
            close(demux);
            close(frontend);
            fclose(output_file);

            // Converte o TS para AAC
            char aac_filename[256]; // Aumentado para evitar truncamento
            create_timestamped_filename(aac_filename, sizeof(aac_filename), "aac");
            convert_ts_to_aac(ts_filename, aac_filename);

            // Compressão do arquivo TS
            compress_file(ts_filename);
            char compressed_ts_filename[260]; // Aumentado para evitar truncamento
            snprintf(compressed_ts_filename, sizeof(compressed_ts_filename), "%s.gz", ts_filename);

            // Compressão do arquivo AAC
            compress_file(aac_filename);
            char compressed_aac_filename[260]; // Aumentado para evitar truncamento
            snprintf(compressed_aac_filename, sizeof(compressed_aac_filename), "%s.gz", aac_filename);
            
            // Enviar os arquivos para o drive
            upload_to_drive(compressed_ts_filename, "tsSaveBackup");
            upload_to_drive(compressed_aac_filename, "tsSaveBackup/Audio");

            // Deletar o arquivo TS original e os arquivos comprimidos
            printf("Deleting files...\n");
            printf("TS: %s\n", ts_filename);
            delete_file(ts_filename);
            printf("Compressed TS: %s\n", compressed_ts_filename);
            delete_file(compressed_ts_filename);
            printf("Compressed AAC: %s\n", compressed_aac_filename);
            delete_file(compressed_aac_filename);

            current_node = current_node->next;
            printf("Current Channel: %s\n", current_node->data.name);

            if ((frontend = open(FRONTEND_PATH, O_RDWR | O_NONBLOCK)) < 0) {
                perror("FRONTEND OPEN: ");
                return -1;
            }

            if (tune(frontend, current_node->data.frequency) < 0) {
                perror("Não foi possível sintonizar o canal.");
                return -1;
            }

            if ((demux = open(DEMUX_PATH, O_RDWR | O_NONBLOCK)) < 0) {
                perror("Couldn't open demux: ");
                return -1;
            }

            if (setup_demux(demux) < 0) {
                perror("Não foi possível configurar demux");
                return -1;
            }

            tuneClock = currentTimeMillis();
            while ((currentTimeMillis() - tuneClock) < WAIT_TIME) {
            }

            if ((dvr = open(DVR_PATH, O_RDONLY)) < 0) {
                perror("DVR DEVICE: ");
                return -1;
            }

            if (PlayerInit(&player) < 0) {
                printf("Player Init Failed\n");
                return -1;
            }

            PlayerStart(&player);

            create_timestamped_filename(ts_filename, sizeof(ts_filename), "ts");

            // Abrir o novo arquivo para salvar os dados recebidos
            output_file = fopen(ts_filename, "wb");
            if (!output_file) {
                perror("Could not open output file");
                return -1;
            }

            start_time = currentTimeMillis();
        }

        unsigned char data_buffer[BUFFER_SIZE];
        unsigned int bytes_read = read(dvr, data_buffer, BUFFER_SIZE);

        if (bytes_read > 0) {
            // Injeta os dados no player
            InjectData(&player, data_buffer, bytes_read);

            // Escreve os dados no arquivo de saída
            size_t bytes_written = fwrite(data_buffer, 1, bytes_read, output_file);
            if (bytes_written != bytes_read) {
                perror("Failed to write data to output file");
                break;
            }
        } else if (bytes_read == 0) {
            printf("No bytes read\n");
        } else {
            perror("Error when reading bytes from DVR");
            break;
        }
    }

    // Restaurar as configurações do terminal
    tcsetattr(STDIN_FILENO, TCSANOW, &termios_original);

    // Liberar recursos
    PlayerFree(&player);
    free_channel_list(&channel_list);

    // Fechar descritores de arquivos
    close(demux);
    close(frontend);
    close(dvr);

    // Fechar arquivo de saída
    fclose(output_file);

    // Converter o último arquivo TS para AAC, comprimir, enviar para o drive e deletar os arquivos
    char aac_filename[256];
    create_timestamped_filename(aac_filename, sizeof(aac_filename), "aac");
    convert_ts_to_aac(ts_filename, aac_filename);

    compress_file(ts_filename);
    compress_file(aac_filename);
    
    char compressed_ts_filename[260];
    snprintf(compressed_ts_filename, sizeof(compressed_ts_filename), "%s.gz", ts_filename);

    char compressed_aac_filename[260];
    snprintf(compressed_aac_filename, sizeof(compressed_aac_filename), "%s.gz", aac_filename);

    upload_to_drive(compressed_ts_filename, "tsSaveBackup");
    upload_to_drive(compressed_aac_filename, "tsSaveBackup/Audio");

    delete_file(ts_filename);
    delete_file(compressed_ts_filename);
    delete_file(compressed_aac_filename);

    return 0;
}
