/*
 * http_client.cpp - Implementação básica de cliente HTTP
 *
 * Objetivo: Aprendizagem do protocolo HTTP
 *
 * Compilar: g++ -std=c++17 -Wall http_client.cpp -o http_client
 * Executar: ./http_client <URL> [método] [--data <dados>] [--headers <headers>]
 *
 * Exemplos:
 *   ./http_client http://httpbin.org/get
 *   ./http_client http://httpbin.org/post POST --data '{"teste": "dados"}'
 *   ./http_client http://httpbin.org/get --headers "Authorization: Bearer token"
 */

#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <regex>
#include <map>
#include <iomanip>

class HTTPClient {
private:
    std::string user_agent;

public:
    HTTPClient() : user_agent("CustomHTTPClient/1.0") {}

    struct HTTPResponse {
        std::string version;
        int status_code;
        std::string status_text;
        std::map<std::string, std::string> headers;
        std::string body;
        size_t content_length;

        HTTPResponse() : status_code(0), content_length(0) {}
    };

    struct URL {
        std::string protocol;
        std::string host;
        int port;
        std::string path;
        std::string query;

        URL() : port(80) {}

        bool parse(const std::string& url) {
            // Regex para parsing de URL
            std::regex url_regex(R"((https?)://([^:/]+)(?::(\d+))?(/[^?#]*)?(\?[^#]*)?)");
            std::smatch matches;

            if (!std::regex_match(url, matches, url_regex)) {
                std::cerr << "URL inválida: " << url << std::endl;
                return false;
            }

            protocol = matches[1];
            host = matches[2];

            // Porta padrão baseada no protocolo
            if (matches[3].matched && !matches[3].str().empty()) {
                port = std::stoi(matches[3]);
            } else {
                port = (protocol == "https") ? 443 : 80;
            }

            path = matches[4].matched ? matches[4].str() : "/";
            query = matches[5].matched ? matches[5].str() : "";

            return true;
        }
    };

    HTTPResponse request(const std::string& method, const std::string& url_str,
                        const std::string& body = "",
                        const std::map<std::string, std::string>& custom_headers = {}) {
        HTTPResponse response;

        // Parse da URL
        URL url;
        if (!url.parse(url_str)) {
            return response;
        }

        // Criar socket
        int sock = create_socket(url.host, url.port);
        if (sock < 0) {
            return response;
        }

        // Construir requisição HTTP
        std::string request = build_http_request(method, url, body, custom_headers);

        // Enviar requisição
        if (send(sock, request.c_str(), request.length(), 0) < 0) {
            std::cerr << "Erro ao enviar requisição" << std::endl;
            close(sock);
            return response;
        }

        // Receber resposta
        response = receive_http_response(sock);
        close(sock);

        return response;
    }

private:
    int create_socket(const std::string& host, int port) {
        // Resolver nome do host
        struct hostent* server = gethostbyname(host.c_str());
        if (!server) {
            std::cerr << "Erro ao resolver host: " << host << std::endl;
            return -1;
        }

        // Criar socket
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            std::cerr << "Erro ao criar socket" << std::endl;
            return -1;
        }

        // Configurar endereço do servidor
        struct sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);
        memcpy(&server_addr.sin_addr, server->h_addr, server->h_length);

        // Conectar
        if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            std::cerr << "Erro ao conectar com " << host << ":" << port << std::endl;
            close(sock);
            return -1;
        }

        // Timeout de recepção
        struct timeval timeout{5, 0}; // 5 segundos
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        return sock;
    }

    std::string build_http_request(const std::string& method, const URL& url,
                                  const std::string& body,
                                  const std::map<std::string, std::string>& custom_headers) {
        std::stringstream request;

        // Linha de requisição
        request << method << " " << url.path << url.query << " HTTP/1.1\r\n";

        // Headers básicos
        request << "Host: " << url.host << "\r\n";
        request << "User-Agent: " << user_agent << "\r\n";
        request << "Connection: close\r\n";

        // Headers customizados
        for (const auto& header : custom_headers) {
            request << header.first << ": " << header.second << "\r\n";
        }

        // Content-Length se tiver body
        if (!body.empty()) {
            request << "Content-Length: " << body.length() << "\r\n";
        }

        // Fim dos headers
        request << "\r\n";

        // Body
        if (!body.empty()) {
            request << body;
        }

        return request.str();
    }

    HTTPResponse receive_http_response(int sock) {
        HTTPResponse response;
        char buffer[4096];
        std::string raw_response;

        // Receber dados até conexão fechar
        while (true) {
            ssize_t bytes_received = recv(sock, buffer, sizeof(buffer) - 1, 0);
            if (bytes_received <= 0) {
                break;
            }

            buffer[bytes_received] = '\0';
            raw_response.append(buffer, bytes_received);
        }

        if (raw_response.empty()) {
            std::cerr << "Nenhuma resposta recebida" << std::endl;
            return response;
        }

        // Parse da resposta
        parse_http_response(raw_response, response);

        return response;
    }

    void parse_http_response(const std::string& raw_response, HTTPResponse& response) {
        std::istringstream stream(raw_response);
        std::string line;

        // Linha de status
        if (std::getline(stream, line)) {
            parse_status_line(line, response);
        }

        // Headers
        while (std::getline(stream, line) && line != "\r" && !line.empty()) {
            parse_header_line(line, response);
        }

        // Body
        size_t header_end = raw_response.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            response.body = raw_response.substr(header_end + 4);
        }

        // Tratar chunked encoding
        if (response.headers.count("Transfer-Encoding") &&
            response.headers["Transfer-Encoding"] == "chunked") {
            response.body = parse_chunked_body(response.body);
        }
    }

    void parse_status_line(const std::string& line, HTTPResponse& response) {
        std::istringstream ss(line);
        ss >> response.version >> response.status_code;

        // Ler texto do status (resto da linha)
        std::getline(ss, response.status_text);

        // Remover espaços em branco do início/fim
        response.status_text = std::regex_replace(response.status_text,
                                                std::regex("^\\s+|\\s+$"), "");
    }

    void parse_header_line(const std::string& line, HTTPResponse& response) {
        size_t colon_pos = line.find(':');
        if (colon_pos != std::string::npos) {
            std::string key = line.substr(0, colon_pos);
            std::string value = line.substr(colon_pos + 1);

            // Remover espaços em branco
            key = std::regex_replace(key, std::regex("^\\s+|\\s+$"), "");
            value = std::regex_replace(value, std::regex("^\\s+|\\s+$"), "");

            response.headers[key] = value;

            // Extrair Content-Length
            if (key == "Content-Length") {
                response.content_length = std::stoul(value);
            }
        }
    }

    std::string parse_chunked_body(const std::string& chunked_body) {
        std::string result;
        std::istringstream stream(chunked_body);
        std::string line;

        while (std::getline(stream, line)) {
            // Tamanho do chunk em hexadecimal
            size_t chunk_size;
            std::stringstream ss;
            ss << std::hex << line;
            ss >> chunk_size;

            if (chunk_size == 0) {
                break; // Fim dos chunks
            }

            // Ler dados do chunk
            std::string chunk_data;
            chunk_data.resize(chunk_size);
            stream.read(&chunk_data[0], chunk_size);

            result += chunk_data;

            // Pular \r\n após o chunk
            stream.ignore(2);
        }

        return result;
    }
};

void print_usage() {
    std::cout << "Uso: http_client <URL> [método] [opções]\n";
    std::cout << "Métodos: GET, POST, PUT, DELETE, HEAD (padrão: GET)\n";
    std::cout << "Opções:\n";
    std::cout << "  --data <dados>      Dados para POST/PUT\n";
    std::cout << "  --headers <header>  Headers adicionais (ex: \"Authorization: Bearer token\")\n";
    std::cout << "  --help              Mostrar esta ajuda\n";
}

void print_response(const HTTPClient::HTTPResponse& response, bool show_headers = true) {
    std::cout << "=== RESPOSTA HTTP ===\n";
    std::cout << response.version << " " << response.status_code << " " << response.status_text << "\n";

    if (show_headers) {
        std::cout << "\n--- HEADERS ---\n";
        for (const auto& header : response.headers) {
            std::cout << header.first << ": " << header.second << "\n";
        }
    }

    std::cout << "\n--- BODY ---\n";
    if (!response.body.empty()) {
        // Tentar identificar e formatar JSON
        if (response.body[0] == '{' || response.body[0] == '[') {
            // Simples pretty print para JSON
            std::string formatted;
            int indent = 0;
            bool in_string = false;

            for (char c : response.body) {
                if (c == '\"' && (formatted.empty() || formatted.back() != '\\')) {
                    in_string = !in_string;
                }

                if (!in_string) {
                    if (c == '{' || c == '[') {
                        formatted += c;
                        formatted += '\n';
                        indent += 2;
                        formatted += std::string(indent, ' ');
                    } else if (c == '}' || c == ']') {
                        formatted += '\n';
                        indent -= 2;
                        formatted += std::string(indent, ' ');
                        formatted += c;
                    } else if (c == ',') {
                        formatted += c;
                        formatted += '\n';
                        formatted += std::string(indent, ' ');
                    } else if (c == ':') {
                        formatted += ": ";
                    } else {
                        formatted += c;
                    }
                } else {
                    formatted += c;
                }
            }
            std::cout << formatted << "\n";
        } else {
            std::cout << response.body << "\n";
        }
    } else {
        std::cout << "(vazio)\n";
    }

    std::cout << "\n=== ESTATÍSTICAS ===\n";
    std::cout << "Tamanho do conteúdo: " << response.body.length() << " bytes\n";
    if (response.content_length > 0) {
        std::cout << "Content-Length: " << response.content_length << " bytes\n";
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string url = argv[1];
    std::string method = "GET";
    std::string data;
    std::map<std::string, std::string> headers;

    // Parse argumentos
    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "--help") {
            print_usage();
            return 0;
        } else if (arg == "--data" && i + 1 < argc) {
            data = argv[++i];
            if (method == "GET") method = "POST"; // Default para POST se tiver dados
        } else if (arg == "--headers" && i + 1 < argc) {
            std::string header_line = argv[++i];
            size_t colon_pos = header_line.find(':');
            if (colon_pos != std::string::npos) {
                std::string key = header_line.substr(0, colon_pos);
                std::string value = header_line.substr(colon_pos + 1);
                headers[key] = value;
            }
        } else if (arg == "GET" || arg == "POST" || arg == "PUT" ||
                  arg == "DELETE" || arg == "HEAD") {
            method = arg;
        } else {
            std::cerr << "Argumento desconhecido: " << arg << std::endl;
            print_usage();
            return 1;
        }
    }

    // Headers padrão para JSON se dados forem JSON-like
    if (!data.empty() && (data[0] == '{' || data[0] == '[')) {
        if (headers.find("Content-Type") == headers.end()) {
            headers["Content-Type"] = "application/json";
        }
    }

    HTTPClient client;

    std::cout << "Enviando requisição " << method << " para " << url << std::endl;
    if (!data.empty()) {
        std::cout << "Com dados: " << data << std::endl;
    }

    auto start = std::chrono::steady_clock::now();
    auto response = client.request(method, url, data, headers);
    auto end = std::chrono::steady_clock::now();

    if (response.status_code == 0) {
        std::cerr << "Erro na requisição HTTP" << std::endl;
        return 1;
    }

    print_response(response);

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    std::cout << "Tempo total: " << duration.count() << "ms\n";

    return 0;
}
