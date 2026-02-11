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
        print("--- Test 2: SET e GET ---")
        key = "mykey"
        value = "myvalue"
        response_set = r.set(key, value)
        print(f"SET '{key}' to '{value}' response: {response_set}")
        assert response_set is True
        time.sleep(0.5)

        response_get = r.get(key)
        print(f"GET '{key}' response: {response_get}")
        assert response_get == value 
        time.sleep(0.5)

        print("--- Test 2.1: GET di chiave non esistente ---")
        non_existent_key = "nonexistentkey"
        response_get_non_existent = r.get(non_existent_key)
        print(f"GET '{non_existent_key}' response: {response_get_non_existent}")
        assert response_get_non_existent is None # Redis ritorna nil (None in Python) per chiavi non esistenti
        time.sleep(0.5)
        print()
        
        # --- TEST 3: Pipeline (semplice) ---
        # print("--- Test 3: Pipeline (semplice) ---")
        # pipe = r.pipeline()
        # pipe.set("key1", "value1")
        # pipe.get("key1")
        # pipe.ping()
        # responses_pipeline = pipe.execute()
        # print(f"Pipeline responses: {responses_pipeline}")
        # assert responses_pipeline == [True, "value1", True]
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