#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <pigpio.h>
#include <signal.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "./src/cjson/cJSON.h"
#include <curl/curl.h>
#include <qrencode.h>

#define PP_BUTTON 23
#define SKIP_BUTTON 22
#define BACK_BUTTON 27
#define ADC_ADDR 0x4b

#include "raylib.h"
#define RAYGUI_IMPLEMENTATION
#include "./src/raygui.h"
#include "./src/raygui/styles/dark/style_dark.h"

#define SCREEN_WIDTH 800
#define SCREEN_HEIGHT 480

volatile int running = 1;
volatile bool logged_in = false;

typedef struct {
    char title[128];
    char artist[128];
    char album[128];
    char url[256]; 
    int progress;
    int duration;
} SongInfo;

typedef enum {
    PLAYLIST,
    ALBUM
} SearchResults;

typedef struct {
    char client_id[256];
    char client_secret[256];
    char access_token[256];
    char refresh_token[256];
    int expiry;
    int refresh_timer;
    int refresh_timeout;
    int volume;
    bool is_playing;
    bool shuffle;
    char current_playing_id[256];
    char current_track_id[256];
} SpotifyClient;

typedef enum {
    STATE_LOGIN,
    STATE_QR_CODE,
    STATE_APP
} AppState;

typedef struct {
    char *memory;
    size_t size;
} MemoryBuffer;

typedef struct {
    char client_id[256];
    char client_secret[256];
    char state[256];
    char redirect_uri[256];
} AuthData;

typedef struct {
    char endpoint[512];
    bool usePost;
    char access_token[256];
} NetCmdData;

SpotifyClient spclient;
pthread_mutex_t spclient_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t logged_in_mutex = PTHREAD_MUTEX_INITIALIZER;

SongInfo current_song = {0};
Texture2D albumTexture = {0};

static Texture2D qrtexture = {0};

size_t write_callback(void *content, size_t size, size_t n, void *user) {
    size_t realsize = size * n;
    MemoryBuffer *mem = (MemoryBuffer *)user;
    char *response = realloc(mem->memory, mem->size + realsize + 1);
    if (!response) {
        free(mem->memory);
        mem->memory = NULL;
        mem->size = 0;
        return 0;
    }
    mem->memory = response;
    memcpy(&(mem->memory[mem->size]), content, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;
    
    return realsize;
}

void* http_server_thread(void *arg) {
    AuthData *adata = (AuthData *)arg;
    int server_fd;
    int client_fd;
    struct sockaddr_in addr;
    int opt = 1;
    socklen_t addrlen = sizeof(addr);
    char buffer[2048] = {0};

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        printf("socket creation failed\n");
        free(adata);
        return NULL;
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("set sock options reuseaddr error");
        free(adata);
        close(server_fd);
        return NULL;
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt))) {
        perror("set sock options reuseport error");
        free(adata);
        close(server_fd);
        return NULL;
    }

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8889);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind failed");
        free(adata);
        close(server_fd);
        return NULL;
    }

    if (listen(server_fd, 3) < 0) {
        perror("listen failed");
        free(adata);
        close(server_fd);
        return NULL;
    }

    if ((client_fd = accept(server_fd, (struct sockaddr *)&addr, &addrlen)) < 0) {
        perror("accept failed");
        free(adata);
        close(server_fd);
        return NULL;
    }

    read(client_fd, buffer, sizeof(buffer) - 1);

    char *code = NULL;
    char *state = NULL;
    char *query = strstr(buffer, "GET /callback?");
    if (query) {
        query += strlen("GET /callback?");
        char *end = strstr(query, " ");
        if (end) {
            *end = '\0';
        }

        char *param = strtok(query, "&");
        while (param != NULL) {
            if (strncmp(param, "code=", 5) == 0) {
                code = param + 5;
            } else if (strncmp(param, "state=", 6) == 0) {
                state = param + 6;
            }

            param = strtok(NULL, "&");
        }
    }

    if (code == NULL || state == NULL || strcmp(state, adata->state) != 0) {
        fprintf(stderr, "code or state is null\n");
        goto cleanup;
    }

    CURL *curl = curl_easy_init();
    if (curl) {
        MemoryBuffer response = {
            malloc(1),
            0
        };
        if (!response.memory) {
            return NULL;
        }

        char pdata[1024];
        snprintf(pdata, sizeof(pdata),
            "grant_type=authorization_code&"
            "code=%s&"
            "redirect_uri=%s&"
            "client_id=%s&"
            "client_secret=%s",
            code, adata->redirect_uri, adata->client_id, adata->client_secret);
             
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");

        curl_easy_setopt(curl, CURLOPT_URL, "https://accounts.spotify.com/api/token");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, pdata);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&response);
        
        CURLcode res = curl_easy_perform(curl);
        if (res == CURLM_OK && response.size != 0) {
            cJSON *json = cJSON_Parse(response.memory);
            if (json) {
                cJSON *access_token_json = cJSON_GetObjectItemCaseSensitive(json, "access_token");
                cJSON *refresh_token_json = cJSON_GetObjectItemCaseSensitive(json, "refresh_token");
                cJSON *expires_in_json = cJSON_GetObjectItemCaseSensitive(json, "expires_in");

                if (cJSON_IsString(access_token_json) && cJSON_IsString(refresh_token_json) &&
                    cJSON_IsNumber(expires_in_json)) {
                    pthread_mutex_lock(&spclient_mutex);
                    strncpy(spclient.access_token, access_token_json->valuestring, sizeof(spclient.access_token));
                    spclient.access_token[sizeof(spclient.access_token) - 1] = '\0';
                    strncpy(spclient.refresh_token, refresh_token_json->valuestring, sizeof(spclient.refresh_token));
                    spclient.refresh_token[sizeof(spclient.refresh_token) - 1] = '\0';
                    spclient.expiry = expires_in_json->valueint;
                    strncpy(spclient.client_id, adata->client_id, sizeof(spclient.client_id));
                    spclient.client_id[sizeof(spclient.client_id) - 1] = '\0';
                    strncpy(spclient.client_secret, adata->client_secret, sizeof(spclient.client_secret));
                    spclient.client_secret[sizeof(spclient.client_secret) - 1] = '\0';
                    pthread_mutex_unlock(&spclient_mutex);

                    pthread_mutex_lock(&logged_in_mutex);
                    logged_in = true;
                    pthread_mutex_unlock(&logged_in_mutex);
                }
                cJSON_Delete(json);
            }
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }

    cleanup:
        char *http_response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n<html><body>Login successful! You can close this window.</body></html>";
        send(client_fd, http_response, strlen(http_response), 0);
        close(client_fd);
        close(server_fd);
        free(adata);
        return NULL;
}

bool spotify_request(const char *endpoint_base, const char *access_token) {
    char endpoint[512];
    pthread_mutex_lock(&spclient_mutex);
    if (strlen(spclient.current_playing_id) > 0) {
        snprintf(endpoint, sizeof(endpoint), "%s?device_id=%s", endpoint_base, spclient.current_playing_id);
    } else {
        strncpy(endpoint, endpoint_base, sizeof(endpoint) - 1);
        endpoint[sizeof(endpoint) - 1] = '\0';
    }
    pthread_mutex_unlock(&spclient_mutex);

    CURL *curl = curl_easy_init();
    if (!curl) {
        return false;
    }

    struct curl_slist *headers = NULL;
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", access_token);
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, endpoint);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        
    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK);
}

bool fetch_current_state(SongInfo *song) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        return false;
    }

    MemoryBuffer region = {
        malloc(1),
        0
    };
    if (!region.memory) {
        return false;
    }

    char url[256];
    snprintf(url, sizeof(url), "https://api.spotify.com/v1/me/player");

    pthread_mutex_lock(&spclient_mutex);
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", spclient.access_token);
    pthread_mutex_unlock(&spclient_mutex);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&region);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK || region.size == 0) {
        free(region.memory);
        return false;
    }

    cJSON *json = cJSON_Parse(region.memory);
    free(region.memory);
    if (!json) {
        return false;
    }

    cJSON *err = cJSON_GetObjectItem(json, "error");
    if (err) {
        cJSON_Delete(json);
        return false;
    }

    cJSON *device_json = cJSON_GetObjectItem(json, "device");
    if (device_json) {
        cJSON *device_id_json = cJSON_GetObjectItem(json, "id");
        if (cJSON_IsString(device_id_json)) {
            pthread_mutex_lock(&spclient_mutex);
            strncpy(spclient.current_playing_id, device_id_json->valuestring, sizeof(spclient.current_playing_id));
            spclient.current_playing_id[sizeof(spclient.current_playing_id) - 1] = '\0';
            pthread_mutex_unlock(&spclient_mutex);
        }
    }

    cJSON *is_playing_json = cJSON_GetObjectItem(json, "is_playing");
    if (cJSON_IsBool(is_playing_json)) {
        spclient.is_playing = cJSON_IsTrue(is_playing_json);
    }

    cJSON *progress_ms_json = cJSON_GetObjectItem(json, "progress_ms");
    song->progress = (cJSON_IsNumber(progress_ms_json)) ? progress_ms_json->valueint / 1000 : 0;

    cJSON *item = cJSON_GetObjectItem(json, "item");
    if (item) {
        cJSON *title_json = cJSON_GetObjectItem(item, "name");
        if (cJSON_IsString(title_json)) {
            strncpy(song->title, title_json->valuestring, sizeof(song->title));
            song->title[sizeof(song->title) - 1] = '\0';
        }
        
        cJSON *track_id_json = cJSON_GetObjectItem(item, "id");
        if (cJSON_IsString(track_id_json)) {
            pthread_mutex_lock(&spclient_mutex);
            strncpy(spclient.current_track_id, track_id_json->valuestring, 
                sizeof(spclient.current_track_id));
            spclient.current_track_id[sizeof(spclient.current_track_id) - 1] = '\0';
            pthread_mutex_unlock(&spclient_mutex);
        }

        cJSON *duration_json = cJSON_GetObjectItem(item, "duration_ms");
        song->duration = (cJSON_IsNumber(duration_json)) ? duration_json->valueint / 1000 : 0;

        cJSON *album = cJSON_GetObjectItem(item, "album");
        if (album) {
            cJSON *album_name_json = cJSON_GetObjectItem(album, "name");
            if (cJSON_IsString(album_name_json)) {
                strncpy(song->album, album_name_json->valuestring, sizeof(song->album));
                song->album[sizeof(song->album) - 1] = '\0';
            }

            cJSON *images_json = cJSON_GetObjectItem(album, "images");
            if (images_json && cJSON_IsArray(images_json)) {
                cJSON *first_image = cJSON_GetArrayItem(images_json, 0);
                if (first_image) {
                    cJSON *image_url_json = cJSON_GetObjectItem(first_image, "url");
                    if (cJSON_IsString(image_url_json)) {
                        strncpy(song->url, image_url_json->valuestring, sizeof(song->url));
                        song->url[sizeof(song->url) - 1] = '\0';
                    }
                }
            }
        }

        cJSON *artists = cJSON_GetObjectItem(item, "artists");
        if (artists && cJSON_IsArray(artists)) {
            cJSON *first_artist_json = cJSON_GetArrayItem(artists, 0);
            if (first_artist_json) {
                cJSON *arist_name_json = cJSON_GetObjectItem(first_artist_json, "name");
                if (cJSON_IsString(arist_name_json)) {
                    strncpy(song->artist, arist_name_json->valuestring, sizeof(song->artist));
                    song->artist[sizeof(song->artist) - 1] = '\0';
                }
            }
        }
    }
    cJSON_Delete(json);

    return true;
}

Texture2D load_album_art(const char *image_url) {
    Texture2D texture = {0};
    CURL *curl = curl_easy_init();
    if (!curl) {
        return texture;
    }

    MemoryBuffer imgBuffer = {
        malloc(1),
        0
    };
    if (!imgBuffer.memory) {
        return texture;
    }

    curl_easy_setopt(curl, CURLOPT_URL, image_url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&imgBuffer);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res == CURLE_OK && imgBuffer.size > 0) {
        Image img = LoadImageFromMemory(".jpg", (unsigned char *)imgBuffer.memory, imgBuffer.size);
        if (img.data) {
            ImageResize(&img, 160, 160);
            texture = LoadTextureFromImage(img);
            UnloadImage(img);
        }
    }

    free(imgBuffer.memory);
    return texture;
}

void refresh_album_art() {
    if (albumTexture.id != 0) {
        UnloadTexture(albumTexture);
        albumTexture = (Texture2D){0};
    }
}

bool spotify_post(const char *endpoint_base, const char *access_token) {
    char endpoint[512];
    pthread_mutex_lock(&spclient_mutex);
    if (strlen(spclient.current_playing_id) > 0) {
        snprintf(endpoint, sizeof(endpoint), "%s?device_id=%s", endpoint_base, spclient.current_playing_id);
    } else {
        strncpy(endpoint, endpoint_base, sizeof(endpoint) - 1);
        endpoint[sizeof(endpoint) - 1] = '\0';
    }
    pthread_mutex_unlock(&spclient_mutex);

    CURL *curl = curl_easy_init();
    if (!curl) {
        return false;
    }

    struct curl_slist *headers = NULL;
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", access_token);
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, endpoint);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    long response_code = 0;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NULL);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK && response_code == 204);
}

void* network_thread(void *arg) {
    NetCmdData *cmd = (NetCmdData *)arg;
    if (!cmd) {
        printf("Invalid NetCmdData in network_thread\n");
        return NULL;
    }

    if (strlen(cmd->endpoint) >= sizeof(cmd->endpoint)) {
        printf("Endpoint URL too long\n");
        free(cmd);
        return NULL;
    }

    if (strlen(cmd->access_token) >= sizeof(cmd->access_token)) {
        printf("Access token too long\n");
        free(cmd);
        return NULL;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        printf("Failed to initialize CURL\n");
        free(cmd);
        return NULL;
    }

    struct curl_slist *headers = NULL;
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", cmd->access_token);
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, cmd->endpoint);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    if (cmd->usePost) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    } else {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    }

    long response_code = 0;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NULL);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    // printf("Network request completed: %s, Response Code: %ld\n", cmd->endpoint, response_code);

    free(cmd);
    return NULL;
}

void handle_sigint(int sig) {
    running = 0;
}

int readADC(int handle, int channel) {
    char controlByte = (channel << 4) | 0x80;
    char data[1];

    if (i2cWriteDevice(handle, &controlByte, 1) != 0) {
        return -1;
    }

    if (i2cReadDevice(handle, data, 1) != 1) {
        return -1;
    }

    return (int)data[0];
}

void buttonPressed(int gpio, int level, uint32_t tick) {
    static uint32_t lastTick = 0;
    if (tick - lastTick < 100000) return;
    lastTick = tick;

    if (level == PI_LOW) {
        // printf("Button pressed: GPIO %d\n", gpio);
        char access_token_copy[256];

        NetCmdData *cmd = malloc(sizeof(NetCmdData));
        if (!cmd) {
            printf("Failed to allocate memory for NetCmdData\n");
            return;
        }

        pthread_mutex_lock(&spclient_mutex);
        bool current_playing = spclient.is_playing;
        if (strlen(spclient.access_token) >= sizeof(cmd->access_token)) {
            printf("Access token too long\n");
            pthread_mutex_unlock(&spclient_mutex);
            free(cmd);
            return;
        }
        strncpy(access_token_copy, spclient.access_token, sizeof(access_token_copy));
        access_token_copy[sizeof(access_token_copy) - 1] = '\0';

        memset(cmd, 0, sizeof(NetCmdData));
        strncpy(cmd->access_token, access_token_copy, sizeof(cmd->access_token));
        cmd->access_token[sizeof(cmd->access_token) - 1] = '\0';
        pthread_mutex_unlock(&spclient_mutex);

        const char* endpoint_url = NULL;
        switch (gpio) {
            case PP_BUTTON:
                endpoint_url = current_playing ?
                    "https://api.spotify.com/v1/me/player/pause" :
                    "https://api.spotify.com/v1/me/player/play";
                cmd->usePost = false;
                break;

            case SKIP_BUTTON:
                endpoint_url = "https://api.spotify.com/v1/me/player/next";
                cmd->usePost = true;
                refresh_album_art();
                fetch_current_state(&current_song);
                // Load new album art immediately
                if (strlen(current_song.url) > 0) {
                    albumTexture = load_album_art(current_song.url);
                }
                break;

            case BACK_BUTTON:
                endpoint_url = "https://api.spotify.com/v1/me/player/previous";
                cmd->usePost = true;
                refresh_album_art();
                fetch_current_state(&current_song);
                // Load new album art immediately
                if (strlen(current_song.url) > 0) {
                    albumTexture = load_album_art(current_song.url);
                }
                break;

            default:
                printf("Unknown GPIO: %d\n", gpio);
                free(cmd);
                break;
        }

        if (strlen(endpoint_url) >= sizeof(cmd->endpoint)) {
            printf("Endpoint URL too long\n");
            free(cmd);
            return;
        }
        strncpy(cmd->endpoint, endpoint_url, sizeof(cmd->endpoint) - 1);
        cmd->endpoint[sizeof(cmd->endpoint) - 1] = '\0';
        // printf("Endpoint: %s\n", cmd->endpoint);

        pthread_t net_thread;
        if (pthread_create(&net_thread, NULL, network_thread, cmd) == 0) {
            pthread_detach(net_thread);
        } else {
            printf("Failed to create network thread\n");
            free(cmd);
        }
    }
}

void* gpio_thread_func(void* arg) {
    if (gpioInitialise() < 0) {
        fprintf(stderr, "Failed to initialize pigpio\n");
        return NULL;
    }

    gpioCfgSetInternals(gpioCfgGetInternals() | PI_CFG_NOSIGHANDLER);

    int i2cHandle = i2cOpen(1, ADC_ADDR, 0);
    if (i2cHandle < 0) {
        fprintf(stderr, "Failed to open I2C\n");
        gpioTerminate();
        return NULL;
    }

    gpioSetMode(PP_BUTTON, PI_INPUT);
    gpioSetPullUpDown(PP_BUTTON, PI_PUD_UP);
    gpioSetMode(SKIP_BUTTON, PI_INPUT);
    gpioSetPullUpDown(SKIP_BUTTON, PI_PUD_UP);
    gpioSetMode(BACK_BUTTON, PI_INPUT);
    gpioSetPullUpDown(BACK_BUTTON, PI_PUD_UP);

    gpioSetAlertFunc(PP_BUTTON, buttonPressed);
    gpioSetAlertFunc(SKIP_BUTTON, buttonPressed);
    gpioSetAlertFunc(BACK_BUTTON, buttonPressed);

    int prev = -1;
    while (running) {
        int values = readADC(i2cHandle, 0);
        if (values < 0) {
            fprintf(stderr, "Error reading ADC\n");
            break;
        }

        if (values != prev) {
            if (values > 0) {
                printf("Volume: %d\n", values);
                prev = values;
            }
        }

        time_sleep(0.1);
    }

    i2cClose(i2cHandle);
    gpioTerminate();
    return NULL;
}

AppState display_qr(AppState currentState) {
    // Center the QR texture on screen
    int texX = (SCREEN_WIDTH - qrtexture.width) / 2;
    int texY = (SCREEN_HEIGHT - qrtexture.height) / 2;
    DrawTexture(qrtexture, texX, texY, WHITE);
    DrawText("Scan QR Code!", SCREEN_WIDTH/2 - 70, texY - 30, 20, WHITE);

    pthread_mutex_lock(&logged_in_mutex);
    if (logged_in) {
        currentState = STATE_APP;
    }
    pthread_mutex_unlock(&logged_in_mutex);

    return currentState;
}

void display_app() {
    bool hasPlayback = fetch_current_state(&current_song);
    static char prev_song_id[256] = {0};

    if (hasPlayback) {
        pthread_mutex_lock(&spclient_mutex);
        char current_track_id[256];
        strncpy(current_track_id, spclient.current_track_id, sizeof(current_track_id));
        current_track_id[sizeof(current_track_id) - 1] = '\0';
        pthread_mutex_unlock(&spclient_mutex);

        if (strcmp(current_track_id, prev_song_id) != 0) {
            refresh_album_art();
            strncpy(prev_song_id, current_track_id, sizeof(prev_song_id));
            prev_song_id[sizeof(prev_song_id) - 1] = '\0';

            if (strlen(current_song.url) > 0) {
                albumTexture = load_album_art(current_song.url);
            }
        }

        if (strlen(current_song.url) > 0 && albumTexture.id == 0) {
            albumTexture = load_album_art(current_song.url);
        }

        if (albumTexture.id != 0) {
            DrawTexture(albumTexture, (SCREEN_WIDTH - albumTexture.width) / 2, 
                (SCREEN_HEIGHT - albumTexture.height) / 2, WHITE);
        }

        char infoText[256];
        snprintf(infoText, sizeof(infoText), "%s - %s", current_song.title, current_song.artist);
        DrawText(infoText, 50, SCREEN_HEIGHT - 120, 20, WHITE);

        float progress_ratio = (current_song.duration > 0) ? (float)current_song.progress / current_song.duration : 0;
        GuiProgressBar((Rectangle){ 50, SCREEN_HEIGHT - 80, SCREEN_WIDTH - 100, 30 }, "", "", &progress_ratio, 0, 1);
        bool current_playing;
        pthread_mutex_lock(&spclient_mutex);
        current_playing = spclient.is_playing;
        pthread_mutex_unlock(&spclient_mutex);

        if (GuiButton((Rectangle){ 50, SCREEN_HEIGHT - 40, 100, 30 }, spclient.is_playing ? "Pause" : "Play")) {
            const char *access_token = spclient.access_token;
            const char *endpoint = current_playing ?
                "https://api.spotify.com/v1/me/player/pause" : "https://api.spotify.com/v1/me/player/play";
            if (spotify_request(endpoint, access_token)) {
                pthread_mutex_lock(&spclient_mutex);
                spclient.is_playing = !spclient.is_playing;
                pthread_mutex_unlock(&spclient_mutex);
            }
        }
    } else {
        // No active playback found, display a prompt and a refresh button.
        DrawText("No active Spotify device found.\nPlease open Spotify on a device.", 
                 SCREEN_WIDTH/2 - 150, SCREEN_HEIGHT/2 - 40, 20, WHITE);
        if (GuiButton((Rectangle){SCREEN_WIDTH/2 - 50, SCREEN_HEIGHT/2 + 20, 100, 40}, "Refresh")) {
            refresh_album_art();
        }
    }
}

int main() {
    signal(SIGINT, handle_sigint);

    pthread_t gpio_t;
    if (pthread_create(&gpio_t, NULL, gpio_thread_func, NULL) != 0) {
        fprintf(stderr, "Failed to start GPIO Thread\n");
        return 1;
    }

    InitWindow(SCREEN_WIDTH, SCREEN_HEIGHT, "Spotify Pi Thing");
    GuiLoadStyleDark();
    SetTargetFPS(60);

    char client_id[256] = {0};
    char client_secret[256] = {0};

    // 0 - none, 1 - client id, 2 - client secret
    int activeText = 0;
    char auth_url[1024] = {0};

    AppState currentState = STATE_LOGIN;

    while (!WindowShouldClose() && running) {
        if (IsKeyPressed(KEY_ESCAPE)) {
            running = 0;
        }

        BeginDrawing();
        ClearBackground(GetColor(GuiGetStyle(DEFAULT, BACKGROUND_COLOR)));

        switch(currentState) {
            case STATE_LOGIN:
                // text is roughly centered above rectangle
                // rectangle is centered plus padding between it and client secret text
                DrawText("Enter Spotify Client ID: ", SCREEN_WIDTH/2 - 125 + 5, SCREEN_HEIGHT/3 - 80, 20, WHITE);
                Rectangle clientIdInput = {SCREEN_WIDTH/2 - 125, SCREEN_HEIGHT/3 - 50, 250, 30};

                // text is roughly centered above rectangle
                // rectangle is centered plus padding between it and spotify login button
                DrawText("Enter Spotify Client Secret: ", SCREEN_WIDTH/2 - 125 - 15, SCREEN_HEIGHT/2 - 80, 20, WHITE);
                Rectangle clientSecretInput = {SCREEN_WIDTH/2 - 125, SCREEN_HEIGHT/2 - 50, 250, 30};

                if (CheckCollisionPointRec(GetMousePosition(), clientIdInput) && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                    activeText = 1;
                }

                if (CheckCollisionPointRec(GetMousePosition(), clientSecretInput) && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                    activeText = 2;
                }

                // no way to validate at the moment so just assuming this works
                if (GetTouchPointCount() > 0) {
                    Vector2 touchPos = GetTouchPosition(0);

                    if (CheckCollisionPointRec(touchPos, clientIdInput)) {
                        activeText = 1;
                    }

                    if (CheckCollisionPointRec(touchPos, clientSecretInput)) {
                        activeText = 2;
                    }
                }

                GuiTextBox(clientIdInput, client_id, 128, activeText == 1);
                GuiTextBox(clientSecretInput, client_secret, 128, activeText == 2);

                // temp for speeding up development
                // auto inserts client id and secret (replace xxxx)
                // !not for production!
                if (GuiLabelButton((Rectangle){SCREEN_WIDTH/2 - 125, SCREEN_HEIGHT/2 + 50, 50, 50}, "Auto")) {
                    strncpy(client_id, "4f3a06ce42164770914c78489a8bd5cf", sizeof(client_id));
                    client_id[sizeof(client_id) - 1] = '\0';

                    strncpy(client_secret, "51bca4d43abb41f1873bf1f22827d364", sizeof(client_secret));
                    client_secret[sizeof(client_secret) - 1] = '\0';
                }

                // once we have input for client id and secret
                // login with spotify button becomes clickable
                // displays authentication qr code to be scanned
                // 
                // if client id and secret align with spotify account
                // spotify pi thing app opens
                // should be about centered
                if (GuiButton((Rectangle){SCREEN_WIDTH/2 - 125, SCREEN_HEIGHT/2, 250, 40}, "Login with Spotify")) {
                    if (strlen(client_id) > 0 && strlen(client_secret) > 0) {
                        AuthData *adata = malloc(sizeof(AuthData));
                        strncpy(adata->client_id, client_id, sizeof(adata->client_id));
                        adata->client_id[sizeof(adata->client_id) - 1] = '\0';
                        strncpy(adata->client_secret, client_secret, sizeof(adata->client_secret));
                        adata->client_secret[sizeof(adata->client_secret) - 1] = '\0';
                        strcpy(adata->redirect_uri, "http://192.168.1.198:8889/callback");

                        srand(time(NULL));
                        snprintf(adata->state, sizeof(adata->state), "%x", rand());

                        snprintf(auth_url, sizeof(auth_url),
                            "https://accounts.spotify.com/authorize?"
                            "response_type=code&"
                            "client_id=%s&"
                            "scope=user-modify-playback-state%%20user-read-playback-state&"
                            "redirect_uri=%s&"
                            "state=%s",
                            adata->client_id, adata->redirect_uri, adata->state);
                        
                        QRcode *qrcode = QRcode_encodeString(auth_url, 0, QR_ECLEVEL_Q, QR_MODE_8, 1);
                        if (qrcode != NULL) {
                            int width = qrcode->width;
                            int scale = 3;
                            int imgwidth = width * scale;
                            Image qrimg = {0};
                            qrimg.width = imgwidth;
                            qrimg.height = imgwidth;
                            qrimg.mipmaps = 1;
                            qrimg.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;

                            unsigned int *pixels = malloc(imgwidth * imgwidth * sizeof(unsigned int));

                            for (int i = 0; i < width; i++) {
                                for (int j = 0; j < width; j++) {
                                    unsigned char module = qrcode->data[i * width + j] & 1;
                                    unsigned color = module ? 0x000000FF : 0xFFFFFFFF;
                                    for (int sy = 0; sy < scale; sy++) {
                                        for (int sx = 0; sx < scale; sx++) {
                                            int px = j * scale + sx;
                                            int py = i * scale + sy;
                                            pixels[py * imgwidth + px] = color;
                                        }
                                    }
                                }
                            }

                            qrimg.data = pixels;
                            qrtexture = LoadTextureFromImage(qrimg);
                            UnloadImage(qrimg);
                            QRcode_free(qrcode);
                            currentState = STATE_QR_CODE;
                        }
                        
                        pthread_t server_thread;
                        pthread_create(&server_thread, NULL, http_server_thread, adata);
                    }
                }
                break;

            case STATE_QR_CODE:
                currentState = display_qr(currentState);
                break;

            case STATE_APP:
                display_app();
                break;
        }

        EndDrawing();
    }

    running = 0;
    pthread_join(gpio_t, NULL);
    if (qrtexture.id != 0) {
        UnloadTexture(qrtexture);
    }
    CloseWindow();

    return 0;
}