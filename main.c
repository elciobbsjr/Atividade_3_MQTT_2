/**
 * @file main.c
 * @brief Núcleo 0 - Controle principal do sistema embarcado com Raspberry Pi Pico W.
 *
 * Este código é executado no núcleo 0 do RP2040 e desempenha as seguintes funções:
 * - Inicialização da interface OLED para exibição de mensagens ao usuário;
 * - Inicialização do PWM para controle de um LED RGB;
 * - Comunicação com o núcleo 1 por meio de FIFO para receber mensagens relacionadas à conexão Wi-Fi;
 * - Exibição e tratamento das mensagens de status do Wi-Fi;
 * - Inicialização do cliente MQTT após o recebimento do IP válido;
 * - Envio periódico da mensagem "PING" via MQTT;
 * - Exibição da confirmação da publicação MQTT recebida do núcleo 1.
 */

#include "fila_circular.h"
#include "rgb_pwm_control.h"
#include "configura_geral.h"
#include "oled_utils.h"
#include "ssd1306_i2c.h"
#include "mqtt_lwip.h"
#include "lwip/ip_addr.h"
#include "pico/multicore.h"
#include <stdio.h>
#include "estado_mqtt.h"

#define INTERVALO_PING_MS 5000  // Intervalo entre envios de "PING" (modificável)

// Declarações externas de funções usadas neste módulo
extern void funcao_wifi_nucleo1(void);
extern void espera_usb();
extern void tratar_ip_binario(uint32_t ip_bin);
extern void tratar_mensagem(MensagemWiFi msg);

// Inicialização de hardware e núcleo 1
void inicia_hardware();
void inicia_core1();

// Manipulação de FIFO e fila de mensagens
void verificar_fifo(void);
void tratar_fila(void);
void inicializar_mqtt_se_preciso(void);
void enviar_ping_periodico(void);

// Estruturas e variáveis globais
FilaCircular fila_wifi;
absolute_time_t proximo_envio;
char mensagem_str[50];
bool ip_recebido = false;

// Função principal
int main() {
    inicia_hardware();
    inicia_core1();

    while (true) {
        verificar_fifo();
        tratar_fila();
        inicializar_mqtt_se_preciso();
        enviar_ping_periodico();
        sleep_ms(50);
    }

    return 0;
}

/*******************************************************************/

// Verifica se há dados no FIFO e trata pacotes recebidos
void verificar_fifo(void) {
    if (!multicore_fifo_rvalid()) return; // Nenhum dado disponível

    uint32_t pacote = multicore_fifo_pop_blocking();
    uint16_t tentativa = pacote >> 16;

    // Se for pacote de IP recebido
    if (tentativa == 0xFFFE) {
        uint32_t ip_bin = multicore_fifo_pop_blocking();
        tratar_ip_binario(ip_bin);
        ip_recebido = true;
        return;
    }

    uint16_t status = pacote & 0xFFFF;

    // Validação de status
    if (status > 2 && tentativa != 0x9999) {
        snprintf(mensagem_str, sizeof(mensagem_str),
                 "Status inválido: %u (tentativa %u)", status, tentativa);

        // Feedback visual e log
        ssd1306_draw_utf8_multiline(buffer_oled, 0, 0, "Status inválido.");
        render_on_display(buffer_oled, &area);
        sleep_ms(3000);
        oled_clear(buffer_oled, &area);
        render_on_display(buffer_oled, &area);

        printf("%s\n", mensagem_str);
        return;
    }

    // Insere a mensagem na fila, se possível
    MensagemWiFi msg = {.tentativa = tentativa, .status = status};
    if (!fila_inserir(&fila_wifi, msg)) {
        // Fila cheia - feedback visual e log
        ssd1306_draw_utf8_multiline(buffer_oled, 0, 0, "Fila cheia. Descartado.");
        render_on_display(buffer_oled, &area);
        sleep_ms(3000);
        oled_clear(buffer_oled, &area);
        render_on_display(buffer_oled, &area);

        printf("Fila cheia. Mensagem descartada.\n");
    }
}

// Retira mensagem da fila e a trata
void tratar_fila(void) {
    MensagemWiFi msg_recebida;
    if (fila_remover(&fila_wifi, &msg_recebida)) {
        tratar_mensagem(msg_recebida);
    }
}

// Inicializa o cliente MQTT se necessário
void inicializar_mqtt_se_preciso(void) {
    if (!mqtt_iniciado && ultimo_ip_bin != 0) {
        printf("[MQTT] Iniciando cliente MQTT...\n");
        iniciar_mqtt_cliente();
        mqtt_iniciado = true;
        proximo_envio = make_timeout_time_ms(INTERVALO_PING_MS);
    }
}

// Envia PING MQTT periodicamente
void enviar_ping_periodico(void) {
    if (mqtt_iniciado && absolute_time_diff_us(get_absolute_time(), proximo_envio) <= 0) {
        publicar_mensagem_mqtt("PING");
        printf("[MQTT] PING publicado\n");
        ssd1306_draw_utf8_multiline(buffer_oled, 0, 0, "PING enviado...");
        render_on_display(buffer_oled, &area);
        proximo_envio = make_timeout_time_ms(INTERVALO_PING_MS);
    }
}

// Inicialização do hardware geral
void inicia_hardware() {
    stdio_init_all();
    setup_init_oled();
    espera_usb();

    oled_clear(buffer_oled, &area);
    render_on_display(buffer_oled, &area);
}

// Inicializa o núcleo 1 e exibe status no OLED
void inicia_core1() {
    ssd1306_draw_utf8_multiline(buffer_oled, 0, 0, "Núcleo 0");
    ssd1306_draw_utf8_multiline(buffer_oled, 0, 16, "Iniciando!");
    render_on_display(buffer_oled, &area);
    sleep_ms(3000);
    oled_clear(buffer_oled, &area);
    render_on_display(buffer_oled, &area);

    printf(">> Núcleo 0 iniciado. Aguardando mensagens do núcleo 1...\n");

    init_rgb_pwm();
    fila_inicializar(&fila_wifi);
    multicore_launch_core1(funcao_wifi_nucleo1);
}
