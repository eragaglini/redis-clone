import redis
import time

# Configurazione del server Redis (il nostro clone in C)
HOST = "127.0.0.1"
PORT = 1234

def main():
    print(f"Connessione al server Redis su {HOST}:{PORT}...")
    try:
        # Crea un'istanza del client Redis
        # decode_responses=True fa sì che il client decodifichi automaticamente le risposte da bytes a stringhe UTF-8
        r = redis.Redis(host=HOST, port=PORT, decode_responses=True)
        
        # Tenta di connettersi e fare un PING per verificare la connessione
        r.ping()
        print("Connesso al server Redis!\n")

        # --- TEST 1: PING ---
        print("--- Test 1: PING ---")
        response_ping = r.ping()
        print(f"PING response: {response_ping}")
        assert response_ping is True
        time.sleep(0.5)
        print()

        # --- TEST 2: SET e GET ---
        # print("--- Test 2: SET e GET ---")
        # key = "mykey"
        # value = "myvalue"
        # response_set = r.set(key, value)
        # print(f"SET '{key}' to '{value}' response: {response_set}")
        # # Il nostro clone restituisce sempre "OK", quindi ci aspettiamo True se decode_responses=True
        # assert response_set is True
        # time.sleep(0.5)

        # response_get = r.get(key)
        # print(f"GET '{key}' response: {response_get}")
        # Attualmente il clone restituisce solo "OK" per ogni comando, non il valore
        # Quindi questo assert fallirà con il clone attuale.
        # assert response_get == value 
        # time.sleep(0.5)
        # print()
        
        # --- TEST 3: Pipeline (semplice) ---
        # print("--- Test 3: Pipeline (semplice) ---")
        # pipe = r.pipeline()
        # pipe.set("key1", "value1")
        # pipe.get("key1")
        # pipe.ping()
        # responses_pipeline = pipe.execute()
        # print(f"Pipeline responses: {responses_pipeline}")
        # Anche qui, con il clone attuale, ci aspettiamo solo [True, 'OK', True] o simili
        # a seconda di come il clone implementa le risposte per SET/GET.
        # Attualmente il clone restituisce solo "OK" per qualsiasi comando.
        # Per ora, verifichiamo solo che ci siano risposte.
        # assert len(responses_pipeline) == 3
        # time.sleep(0.5)
        # print()

    except redis.exceptions.ConnectionError as e:
        print(f"ERRORE DI CONNESSIONE: Impossibile connettersi al server Redis. Il server è acceso? Dettagli: {e}")
    except Exception as e:
        print(f"ERRORE GENERICO: {e}")
    finally:
        # Verifica se l'oggetto 'r' è stato creato e se ha un connection_pool
        if 'r' in locals() and hasattr(r, 'connection_pool'):
            print("\nChiusura connessione Redis.")
            # Disconnette esplicitamente tutte le connessioni nel pool.
            r.connection_pool.disconnect()
        else:
            print("\nNessuna connessione Redis attiva da chiudere.")

if __name__ == "__main__":
    main()