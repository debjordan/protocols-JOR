/*
 * icmp_ping.cpp - Implementação completa de ICMP Echo Request/Reply
 *
 *
 * Compilar: g++ -std=c++17 -Wall icmp_ping.cpp -o icmp_ping
 * Executar: sudo ./icmp 8.8.8.8
 */

 #include <arpa/inet.h>
 #include <cerrno>
 #include <cstring>
 #include <chrono>
 #include <iomanip>
 #include <iostream>
 #include <vector>
 #include <sys/socket.h>
 #include <sys/time.h>
 #include <netinet/ip_icmp.h>
 #include <unistd.h>

 // --- Constantes e Configurações ---
 constexpr int BUFFER_SIZE = 1500;
 constexpr int TIMEOUT_SECONDS = 2;
 constexpr uint16_t DEFAULT_SEQUENCE = 1;

 // --- Estrutura para resultado do ping ---
 struct PingResult {
     bool success;
     double rtt_ms;
     std::string from_addr;
     int bytes_received;
     int ttl;
     uint16_t sequence;
     uint16_t identifier;
 };

 // --- Função de Checksum ICMP ---
 uint16_t icmp_checksum(const void* data, size_t length) {
     const uint16_t* words = static_cast<const uint16_t*>(data);
     uint32_t sum = 0;

     // Soma palavras de 16 bits
     for (size_t i = 0; i < length / 2; ++i) {
         sum += ntohs(words[i]);
     }

     // Trata byte ímpar se necessário
     if (length % 2) {
         const uint8_t* bytes = static_cast<const uint8_t*>(data);
         sum += (bytes[length - 1] << 8);
     }

     // Fold carry bits
     while (sum >> 16) {
         sum = (sum & 0xFFFF) + (sum >> 16);
     }

     return htons(static_cast<uint16_t>(~sum));
 }

 // --- Cria e configura pacote ICMP Echo Request ---
 std::vector<uint8_t> create_icmp_echo_request(uint16_t identifier, uint16_t sequence,
                                             const std::string& payload) {
     struct icmphdr header{};
     header.type = ICMP_ECHO;
     header.code = 0;
     header.un.echo.id = htons(identifier);
     header.un.echo.sequence = htons(sequence);
     header.checksum = 0; // Será calculado depois

     // Monta pacote: header + payload
     std::vector<uint8_t> packet(sizeof(header) + payload.size());

     // Copia header
     memcpy(packet.data(), &header, sizeof(header));

     // Copia payload
     if (!payload.empty()) {
         memcpy(packet.data() + sizeof(header), payload.data(), payload.size());
     }

     // Calcula e define checksum
     uint16_t checksum = icmp_checksum(packet.data(), packet.size());
     memcpy(packet.data() + 2, &checksum, sizeof(checksum)); // Offset do checksum = 2

     return packet;
 }

 // --- Extrai header IP e calcula tamanho ---
 size_t get_ip_header_length(const uint8_t* ip_packet) {
     // Byte 0: Versão (4 bits) + IHL (4 bits)
     uint8_t ihl_words = ip_packet[0] & 0x0F;
     return ihl_words * 4; // Converte para bytes
 }

 // --- Valida resposta ICMP ---
 bool validate_icmp_response(const struct icmphdr* icmp_hdr, uint16_t expected_id,
                           uint16_t expected_seq) {
     if (icmp_hdr->type != ICMP_ECHOREPLY || icmp_hdr->code != 0) {
         return false;
     }

     // IDs devem coincidir (em network byte order)
     return (ntohs(icmp_hdr->un.echo.id) == expected_id) &&
            (ntohs(icmp_hdr->un.echo.sequence) == expected_seq);
 }

 // --- Executa um único ping ---
 PingResult send_ping(int sock, const sockaddr_in& dest,
                     uint16_t identifier, uint16_t sequence) {
     PingResult result{};
     result.sequence = sequence;
     result.identifier = identifier;

     // Prepara payload com timestamp para RTT preciso
     auto send_time = std::chrono::steady_clock::now();
     std::string payload = "PING_PAYLOAD_" + std::to_string(sequence);

     // Cria pacote ICMP
     auto packet = create_icmp_echo_request(identifier, sequence, payload);

     // Envia pacote
     ssize_t sent = sendto(sock, packet.data(), packet.size(), 0,
                          reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
     if (sent <= 0) {
         std::cerr << "Erro no envio: " << strerror(errno) << std::endl;
         return result;
     }

     // Aguarda resposta com timeout
     fd_set read_set;
     FD_ZERO(&read_set);
     FD_SET(sock, &read_set);

     timeval timeout{TIMEOUT_SECONDS, 0};
     int ready = select(sock + 1, &read_set, nullptr, nullptr, &timeout);

     if (ready <= 0) {
         if (ready == 0) {
             std::cout << "Timeout para sequência " << sequence << std::endl;
         } else {
             std::cerr << "Erro no select: " << strerror(errno) << std::endl;
         }
         return result;
     }

     // Recebe resposta
     uint8_t buffer[BUFFER_SIZE];
     sockaddr_in from{};
     socklen_t from_len = sizeof(from);

     ssize_t received = recvfrom(sock, buffer, sizeof(buffer), 0,
                                reinterpret_cast<sockaddr*>(&from), &from_len);
     if (received <= 0) {
         std::cerr << "Erro na recepção: " << strerror(errno) << std::endl;
         return result;
     }

     auto recv_time = std::chrono::steady_clock::now();

     // Processa resposta
     size_t ip_header_len = get_ip_header_length(buffer);

     if (received < static_cast<ssize_t>(ip_header_len + sizeof(icmphdr))) {
         std::cerr << "Pacote recebido muito curto" << std::endl;
         return result;
     }

     // Extrai header ICMP
     struct icmphdr* icmp_response = reinterpret_cast<struct icmphdr*>(buffer + ip_header_len);

     // Valida resposta
     if (!validate_icmp_response(icmp_response, identifier, sequence)) {
         std::cout << "Resposta ICMP inválida ou não esperada" << std::endl;
         return result;
     }

     // Preenche resultado
     result.success = true;
     result.rtt_ms = std::chrono::duration_cast<std::chrono::microseconds>(
         recv_time - send_time).count() / 1000.0;

     char addr_str[INET_ADDRSTRLEN];
     inet_ntop(AF_INET, &from.sin_addr, addr_str, sizeof(addr_str));
     result.from_addr = addr_str;

     result.bytes_received = received - ip_header_len - sizeof(icmphdr);
     result.ttl = buffer[8]; // TTL está no offset 8 do header IP

     return result;
 }

 // --- Função Principal ---
 int main(int argc, char* argv[]) {
     if (argc != 2) {
         std::cerr << "Uso: " << argv[0] << " <endereço_IPv4>" << std::endl;
         return 1;
     }

     // Configura destino
     sockaddr_in destino{};
     destino.sin_family = AF_INET;

     if (inet_pton(AF_INET, argv[1], &destino.sin_addr) != 1) {
         std::cerr << "Endereço IP inválido: " << argv[1] << std::endl;
         return 1;
     }

     // Cria socket RAW
     int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
     if (sock < 0) {
         std::cerr << "Erro ao criar socket RAW: " << strerror(errno) << std::endl;
         std::cerr << "Execute com privilégios de root (sudo)" << std::endl;
         return 1;
     }

     // Garante fechamento do socket ao sair
     auto cleanup = [&]() { close(sock); };

     std::cout << "PING " << argv[1] << " com ID=" << getpid() << std::endl;

     // Executa ping
     uint16_t identifier = static_cast<uint16_t>(getpid() & 0xFFFF);
     auto result = send_ping(sock, destino, identifier, DEFAULT_SEQUENCE);

     // Exibe resultados
     if (result.success) {
         std::cout << "Resposta de " << result.from_addr
                   << ": bytes=" << result.bytes_received
                   << " sequência=" << result.sequence
                   << " TTL=" << result.ttl
                   << " tempo=" << std::fixed << std::setprecision(3)
                   << result.rtt_ms << "ms" << std::endl;
     } else {
         std::cout << "Falha no ping para " << argv[1] << std::endl;
     }

     cleanup();
     return result.success ? 0 : 1;
 }
