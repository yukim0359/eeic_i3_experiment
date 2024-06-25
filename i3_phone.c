#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <math.h>
#include <complex.h>
#include <pthread.h>

#define SERVER 1
#define CLIENT 2
typedef short sample_t;

/* fft関連の関数 */
void sample_to_complex(sample_t * s, 
		       complex double * X, 
		       long n) {
  long i;
  for (i = 0; i < n; i++) X[i] = s[i];
}

void complex_to_sample(complex double * X, 
		       sample_t * s, 
		       long n) {
  long i;
  for (i = 0; i < n; i++) {
    s[i] = creal(X[i]);
  }
}

void fft_r(complex double * x, 
	   complex double * y, 
	   long n, 
	   complex double w) {
  if (n == 1) { y[0] = x[0]; }
  else {
    complex double W = 1.0; 
    long i;
    for (i = 0; i < n/2; i++) {
      y[i]     =     (x[i] + x[i+n/2]); /* 偶数行 */
      y[i+n/2] = W * (x[i] - x[i+n/2]); /* 奇数行 */
      W *= w;
    }
    fft_r(y,     x,     n/2, w * w);
    fft_r(y+n/2, x+n/2, n/2, w * w);
    for (i = 0; i < n/2; i++) {
      y[2*i]   = x[i];
      y[2*i+1] = x[i+n/2];
    }
  }
}

void fft(complex double * x, 
	 complex double * y, 
	 long n) {
  long i;
  double arg = 2.0 * M_PI / n;
  complex double w = cos(arg) - 1.0j * sin(arg);
  fft_r(x, y, n, w);
  for (i = 0; i < n; i++) y[i] /= n;
}

void ifft(complex double * y, 
	  complex double * x, 
	  long n) {
  double arg = 2.0 * M_PI / n;
  complex double w = cos(arg) + 1.0j * sin(arg);
  fft_r(y, x, n, w);
}

/* pthreadのための構造体の定義 */
typedef struct {
  int role;
  int socket;
  FILE *rec;
  FILE *play;
  int len;
  complex double *X;
  complex double *Y;
  long cutoff1;
  long cutoff2;
  long sampling_freq;
  int *mute_flag;
} audio_thread_args_t;

typedef struct {
  int socket;
  int len;
  int *mute_flag;
} send_chat_thread_args_t;

typedef struct {
  int socket;
  int len;
} recv_chat_thread_args_t;

/* 相互音声通話のスレッド */
void* audio_thread(void *args) {
  audio_thread_args_t *audio_args = (audio_thread_args_t*) args;
  int role = audio_args->role;
  int s = audio_args->socket;
  FILE *REC = audio_args->rec;
  FILE *PLAY = audio_args->play;
  int len = audio_args->len;
  complex double *X = audio_args->X;
  complex double *Y = audio_args->Y;
  long cutoff1 = audio_args->cutoff1;
  long cutoff2 = audio_args->cutoff2;
  long sampling_freq = audio_args->sampling_freq;
  int *mute_flag = audio_args->mute_flag;

  short data_rec[len], data_play[len];

  const int left_num = cutoff1 * len / sampling_freq;  // fft後のデータで0にしない左端の番号
  const int right_num = cutoff2 * len / sampling_freq;  // fft後のデータで0にしない右端の番号
  //const int width = right_num - left_num + 1;

  /* mute機能用にwidth分0.0+0.0jが並んだ配列を用意しておく */
  //complex double zeros[width];
  //for(int i=0; i<width; ++i) zeros[i] = 0.0;

  if(role == SERVER){
    while(1){
      // read and send
      int num_data_read = fread(data_rec, sizeof(short), len, REC);
      if (num_data_read == -1) {
        perror("read");
        free(X); free(Y);
        pclose(REC); pclose(PLAY);
        close(s);
        exit(1);
      }
      if (num_data_read == 0) break;
      if( send(s, data_rec, num_data_read * sizeof(short), 0) == -1 ){
        perror("send");
        free(X); free(Y);
        pclose(REC); pclose(PLAY);
        close(s);
        exit(1);
      }

      // receive and bandpass and play
      int bytes_received = recv(s, data_play, len * sizeof(short), 0);
      if (bytes_received == -1) {
        perror("recv");
        free(X); free(Y);
        pclose(REC); pclose(PLAY);
        close(s);
        exit(1);
      }
      if (bytes_received == 0) break;
      /* bandpass */
      sample_to_complex(data_play, X, len);
      fft(X, Y, len);
      for(long l=0; l<len; ++l){
        if(l < cutoff1 * len / sampling_freq || l > cutoff2 * len / sampling_freq) Y[l] = 0.0;
      }
      ifft(Y, X, len);
      complex_to_sample(X, data_play, len);
      fwrite(data_play, sizeof(short), bytes_received/sizeof(short), PLAY);
    }
  }
  if(role == CLIENT){
    while(1){
      // receive and bandpass and play
      int bytes_received = recv(s, data_play, len * sizeof(short), 0);
      if (bytes_received == -1) {
        perror("recv");
        free(X); free(Y);
        pclose(REC); pclose(PLAY);
        close(s);
        exit(1);
      }
      if (bytes_received == 0) break;
      /* bandpass */
      sample_to_complex(data_play, X, len);
      fft(X, Y, len);
      for(long l=0; l<len; ++l){
        if(l < cutoff1 * len / sampling_freq || l > cutoff2 * len / sampling_freq) Y[l] = 0.0;
      }
      ifft(Y, X, len);
      complex_to_sample(X, data_play, len);
      fwrite(data_play, sizeof(short), bytes_received/sizeof(short), PLAY);

      // read and send
      int num_data_read = fread(data_rec, sizeof(short), len, REC);
      if (num_data_read == -1) {
        perror("read");
        free(X); free(Y);
        pclose(REC); pclose(PLAY);
        close(s);
        exit(1);
      }
      if (num_data_read == 0) break;
      if( send(s, data_rec, num_data_read * sizeof(short), 0) == -1 ){
        perror("send");
        free(X); free(Y);
        pclose(REC); pclose(PLAY);
        close(s);
        exit(1);
      }
    }
  }
  return NULL;
}

/* chat送信のスレッド */
void* send_chat_thread(void *args) {
  send_chat_thread_args_t *chat_args = (send_chat_thread_args_t*) args;
  int s = chat_args->socket;
  int len = chat_args->len;
  int *mute_flag = chat_args->mute_flag;
  char send_buf[len+2];
  // send_buf[0] : データの個数を0〜127で表す
  // send_buf[1] ~ send_buf[127] : データ
  // send_buf[128] : 改行，データが127個であるときに末尾に改行を表示する用
  for(int i=0; i<=len; ++i) send_buf[i] = '\0';
  send_buf[len+1] = '\n';

  while(1) {
    char n = read(0, send_buf+1, 127);
    if (n == -1){
      perror("read");
      exit(1);
    } 
    if (n == 0) break;
    send_buf[0] = n;  // bufの先頭文字はデータの個数を表す

    send(s, send_buf, len+2, 0);

    /* mute機能の実装 */
    if( strncmp(send_buf + 1, "!mute", 5) == 0 ) *mute_flag = 1;
    if( strncmp(send_buf + 1, "!unmute", 7) == 0 ) *mute_flag = 0;

    /* ファイル送信機能の実装 */
    if( strncmp(send_buf + 1, "!file", 5) == 0 ){
      int byte_max = 1000;
      char *file_send_buf = (char *) malloc(byte_max * sizeof(char));
      if(file_send_buf == NULL){
        perror("malloc");
        exit(1);
      }
      memset(file_send_buf, -1, byte_max);

      int filename_len = (int)n - 7;  // "!file"の5文字，空白の1文字，改行の1文字を除く
      char filename[filename_len + 1];
      filename[filename_len] = '\0';
      strncpy(filename, send_buf + 7, filename_len);
      FILE *fps = fopen(filename, "rb");
      if(fps == NULL) {
        fprintf(stderr, "--- Error : No such file exists. ---\n");
        if( send(s, file_send_buf, 1, 0) == -1 ){
          perror("send");
          exit(1);
        }
      }
      else{
        int cnt = fread(file_send_buf, sizeof(char), byte_max, fps);
        fclose(fps);
        if(cnt == 0){
          perror("fread");
          exit(1);
        }
        if( send(s, file_send_buf, cnt, 0) == -1 ){
          perror("send");
          exit(1);
        }
        if(cnt >= byte_max){
          fprintf(stderr, "--- Warning : File size is more than %d bytes. ---\n", byte_max);
        }
      }
      free(file_send_buf);
    }

    // bufの初期化
    for(int i=0; i<=len; ++i) send_buf[i] = '\0';
    send_buf[len] = '\n';
  }
  return NULL;
}

void* recv_chat_thread(void *args) {
  recv_chat_thread_args_t *chat_args = (recv_chat_thread_args_t*) args;
  int s = chat_args->socket;
  int len = chat_args->len;
  char mark[5] = "  >> ";  // 返信を表すマーク
  char recv_buf[len+2];
  for(int i=0; i<=len; ++i) recv_buf[i] = '\0';
  recv_buf[len+1] = '\n';

  while(1) {
    char n = recv(s, recv_buf, len+2, 0);
    if (n == -1) {
      perror("recv");
      exit(1);
    }
    if (n == 0) break;
    if(recv_buf[0] != 0){
      write(1, mark, 5);
      if(recv_buf[0] == len && recv_buf[len] != '\n') write(1, recv_buf + 1, len + 1);
      else write(1, recv_buf + 1, recv_buf[0]);
    }

    /* ファイル受信機能の実装 */
    if( strncmp(recv_buf + 1, "!file", 5) == 0 ){
      int byte_max = 1000;
      char *file_recv_buf = (char *) malloc(byte_max * sizeof(char));
      if(file_recv_buf == NULL){
        perror("malloc");
        exit(1);
      }
      memset(file_recv_buf, -1, byte_max);
      int cnt = recv(s, file_recv_buf, byte_max, 0);
      if(file_recv_buf[0] == -1){
        fprintf(stderr, "--- Error : The peer sent a non-exist file. ---\n");
      }
      else{
        int filename_len = (int)recv_buf[0] - 7;  // "!file"の5文字，空白の1文字，改行の1文字を除く
        char filename[filename_len+1];
        filename[filename_len] = '\0';
        strncpy(filename, recv_buf + 7, filename_len);
        FILE *fpr = fopen(filename, "w");
        if(fpr == NULL) {
          perror("can't open the file");
          free(file_recv_buf);
          exit(1);
        }
        fwrite(file_recv_buf, sizeof(char), cnt, fpr);
        fclose(fpr);
        if(cnt >= byte_max){
          fprintf(stderr, "--- Warning : File size is more than %d bytes. ---\n", byte_max);
        }
      }
      free(file_recv_buf);
    }
    // bufの初期化
    for(int i=0; i<=len; ++i) recv_buf[i] = '\0';
  }
  return NULL;
}


int main(int argc, char **argv){
  int role = 0;
  if(argc == 3) role = SERVER;
  if(argc == 4) role = CLIENT;
  if(argc!=3 && argc!=4){
    fprintf(stderr, "usage : %s <port> <port> or %s <IP address> <port> <port> \n", argv[0], argv[0]);
    exit(1);
  }

  int s_a, s_c;

  /* server */
  if(role == SERVER){
    int port_a = atoi(argv[1]);
    int port_c = atoi(argv[2]);

    int ss_a = socket(PF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr_a;
    addr_a.sin_family = AF_INET;
    addr_a.sin_port = htons(port_a);
    addr_a.sin_addr.s_addr = INADDR_ANY;
    if(bind(ss_a, (struct sockaddr *)&addr_a, sizeof(addr_a))<0){
      perror("bind");
      exit(1);
    }
    listen(ss_a, 10);

    int ss_c = socket(PF_INET, SOCK_STREAM, 0);
    if(ss_c == -1){
      perror("socket");
      exit(1);
    }
    struct sockaddr_in addr_c;
    addr_c.sin_family = AF_INET;
    addr_c.sin_port = htons(port_c);
    addr_c.sin_addr.s_addr = INADDR_ANY;
    if(bind(ss_c, (struct sockaddr *)&addr_c, sizeof(addr_c)) < 0){
      perror("bind");
      exit(1);
    }
    if(listen(ss_c, 10) < 0){
      perror("listen");
      exit(1);
    }

    struct sockaddr_in client_addr_a;
    socklen_t len_a = sizeof(struct sockaddr_in);
    struct sockaddr_in client_addr_c;
    socklen_t len_c = sizeof(struct sockaddr_in);
    s_a = accept(ss_a, (struct sockaddr *)&client_addr_a, &len_a);
    if(s_a == -1){
      perror("accept");
      exit(1);
    }
    s_c = accept(ss_c, (struct sockaddr *)&client_addr_c, &len_c);
    if(s_c == -1){
      perror("accept");
      exit(1);
    }
    close(ss_a);
    close(ss_c);
  }

  /* client */
  if(role == CLIENT){
    char *IP_addr = argv[1];
    int port_a = atoi(argv[2]);
    int port_c = atoi(argv[3]);

    s_a = socket(PF_INET, SOCK_STREAM, 0);
    if(s_a == -1){
      perror("socket");
      exit(1);
    }
    struct sockaddr_in addr_a;
    addr_a.sin_family = AF_INET;
    if( inet_aton(IP_addr, &addr_a.sin_addr) == 0 ){
      perror("inet_aton");
      close(s_a);
      exit(1);
    }
    addr_a.sin_port = htons(port_a);

    s_c = socket(PF_INET, SOCK_STREAM, 0);
    if(s_c == -1){
      perror("socket");
      exit(1);
    }
    struct sockaddr_in addr_c;
    addr_c.sin_family = AF_INET;
    if( inet_aton(IP_addr, &addr_c.sin_addr) == 0 ){
      perror("inet_aton");
      close(s_a); close(s_c);
      exit(1);
    }
    addr_c.sin_port = htons(port_c);

    int ret_a = connect(s_a, (struct sockaddr *)&addr_a, sizeof(addr_a));
    if(ret_a == -1){
      perror("connect");
      close(s_a); close(s_c);
      exit(1);
    }
    int ret_c = connect(s_c, (struct sockaddr *)&addr_c, sizeof(addr_c));
    if(ret_c == -1){
      perror("connect");
      close(s_a); close(s_c);
      exit(1);
    }
  }

  int len_audio = 4096;  // 音声データは4096個ずつ送受信する
  int len_chat = 127;  // チャットは127文字までしか送れない
  complex double *X = calloc(sizeof(complex double), len_audio);
  if (X == NULL) {
    perror("calloc");
    exit(1);
  }
  complex double *Y = calloc(sizeof(complex double), len_audio);
  if (Y == NULL) {
    perror("calloc");
    exit(1);
  }
  long cutoff1 = 200;
  long cutoff2 = 3000;
  long sampling_freq = 48000;
  int mute_flag = 0;

  /* start rec */
  FILE *REC = popen("rec -q -t raw -b 16 -c 1 -e s -r 48000 -", "r");
  if(REC == NULL) {
    perror("popen_rec");
    exit(1);
  }

  /* start play */
  FILE *PLAY = popen("play -q -t raw -b 16 -c 1 -e s -r 48000 - 2>/dev/null", "w");
  if(PLAY == NULL) {
    perror("popen_play");
    exit(1);
  }

  audio_thread_args_t audio_args = {role, s_a, REC, PLAY, len_audio, X, Y, cutoff1, cutoff2, sampling_freq, &mute_flag};
  send_chat_thread_args_t send_chat_args = {s_c, len_chat, &mute_flag};
  recv_chat_thread_args_t recv_chat_args = {s_c, len_chat};

  pthread_t audio_tid, send_chat_tid, recv_chat_tid;

  if(pthread_create(&audio_tid, NULL, audio_thread, &audio_args) != 0) {
    perror("pthread_create_audio");
    free(X); free(Y);
    pclose(REC); pclose(PLAY);
    close(s_a); close(s_c);
    exit(1);
  }
  if(pthread_create(&send_chat_tid, NULL, send_chat_thread, &send_chat_args) != 0) {
    perror("pthread_create_send_chat");
    pthread_cancel(audio_tid);
    free(X); free(Y);
    pclose(REC); pclose(PLAY);
    close(s_a); close(s_c);
    exit(1);
  }
  if(pthread_create(&recv_chat_tid, NULL, recv_chat_thread, &recv_chat_args) != 0) {
    perror("pthread_create_recv_chat");
    pthread_cancel(audio_tid); pthread_cancel(send_chat_tid);
    free(X); free(Y);
    pclose(REC); pclose(PLAY);
    close(s_a); close(s_c);
    exit(1);
  }

  pthread_join(audio_tid, NULL);
  pthread_join(send_chat_tid, NULL);
  pthread_join(recv_chat_tid, NULL);

  free(X); free(Y);
  close(s_a); close(s_c);
  pclose(REC); pclose(PLAY);
  return 0;
}