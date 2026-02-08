import socket
import struct
import time

# Configurazione
HOST = "127.0.0.1"
PORT = 1234


def send_msg(sock, args_list):
    """
    Invia un messaggio rispettando il protocollo:
    [4 byte num_args (Little Endian)]
    [4 byte arg1_len (Little Endian)] + [Corpo arg1]
    [4 byte arg2_len (Little Endian)] + [Corpo arg2]
    ...
    """
    if not isinstance(args_list, list):
        raise TypeError("args_list deve essere una lista di stringhe.")

    # 1. Prepara il numero di argomenti
    num_args = len(args_list)
    packed_num_args = struct.pack("<I", num_args)

    full_message_parts = [packed_num_args]

    # 2. Prepara ogni argomento
    for arg in args_list:
        arg_bytes = str(arg).encode(
            "utf-8"
        )  # Assicurati che l'argomento sia una stringa
        arg_len = len(arg_bytes)
        packed_arg_len = struct.pack("<I", arg_len)
        full_message_parts.append(packed_arg_len)
        full_message_parts.append(arg_bytes)

    full_message = b"".join(full_message_parts)

    sock.sendall(full_message)
    print(f"-> Inviato Comando: {args_list}")


def read_n_bytes(sock, n):
    """
    Helper per leggere esattamente n bytes.
    Necessario perché TCP può frammentare i pacchetti.
    """
    chunks = []
    bytes_recd = 0
    while bytes_recd < n:
        chunk = sock.recv(min(n - bytes_recd, 4096))
        if chunk == b"":
            raise RuntimeError("Connessione chiusa dal server")
        chunks.append(chunk)
        bytes_recd += len(chunk)
    return b"".join(chunks)


def read_msg(sock):
    """
    Legge una risposta dal server rispettando il protocollo.
    """
    try:
        # 1. Leggiamo i 4 byte dell'header
        header = read_n_bytes(sock, 4)

        # 2. Scompattiamo la lunghezza (unpack ritorna una tupla, prendiamo il primo elemento)
        (length,) = struct.unpack("<I", header)

        # 3. Leggiamo esattamente 'length' bytes per il corpo
        body_bytes = read_n_bytes(sock, length)

        response = body_bytes.decode("utf-8")
        print(f"<- Ricevuto: '{response}'")
        return response
    except RuntimeError:
        print("<- Errore: Server disconnesso prematuramente")
        return None


def main():
    print(f"Connessione a {HOST}:{PORT}...")
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect((HOST, PORT))
        print("Connesso!\n")

        # --- TEST 1: Richiesta Singola ---
        print("--- Test 1: Richiesta Singola ---")
        send_msg(s, ["PING"])
        read_msg(s)
        time.sleep(1)  # Pausa scenica
        print()

        # --- TEST 2: Pipeline di Comandi (Stress Test per il Buffer) ---
        # Inviamo due comandi validi attaccati SENZA aspettare la risposta in mezzo.
        print("--- Test 2: Pipeline di Comandi (Due comandi attaccati) ---")

        # Inviamo un comando SET
        send_msg(s, ["SET", "mykey", "myvalue"])
        # Inviamo un comando GET
        send_msg(s, ["GET", "mykey"])

        # Ora dovremmo ricevere DUE risposte "OK"
        print("Attendo prima risposta...")
        read_msg(s)
        print("Attendo seconda risposta...")
        read_msg(s)

    except ConnectionRefusedError:
        print("ERRORE: Impossibile connettersi. Il server è acceso?")
    except Exception as e:
        print(f"ERRORE: {e}")
    finally:
        s.close()
        print("\nConnessione chiusa.")


if __name__ == "__main__":
    main()
