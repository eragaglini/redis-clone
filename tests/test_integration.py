import pytest
import subprocess
import time
import redis
import os
import signal
import sys

# Configurazione del server Redis (il nostro clone in C)
HOST = "127.0.0.1"
PORT = 1234
SERVER_PATH = "./bin/main"

@pytest.fixture(scope="module")
def c_server():
    """
    Fixture to start and stop the C Redis clone server.
    """
    # Start the server process
    # Use preexec_fn=os.setsid to create a new process group
    # This ensures that all child processes are killed when the server process is terminated
    server_process = subprocess.Popen(
        [SERVER_PATH],
        preexec_fn=os.setsid, # Start in a new session for easier killing
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True # Decode stdout/stderr as text
    )
    print(f"\nStarted C server with PID: {server_process.pid}")

    # Wait for the server to be ready
    # We'll try to connect to it in a loop
    max_retries = 10
    for i in range(max_retries):
        try:
            r = redis.Redis(host=HOST, port=PORT, decode_responses=True)
            r.ping()
            print(f"Server is ready after {i+1} retries.")
            break
        except redis.exceptions.ConnectionError:
            print(f"Waiting for server to start... (Attempt {i+1}/{max_retries})")
            time.sleep(0.5)
    else:
        # If connection failed after max_retries
        server_process.kill()
        server_process.wait(timeout=1)
        stdout, stderr = server_process.communicate()
        print(f"Server stdout:\n{stdout}")
        print(f"Server stderr:\n{stderr}")
        pytest.fail("C server did not start in time.")

    yield server_process # Yield control to tests

    # Teardown: Stop the server process
    print(f"\nStopping C server with PID: {server_process.pid}...")
    try:
        # Use os.killpg to kill the process group, ensuring all children are terminated
        os.killpg(os.getpgid(server_process.pid), signal.SIGTERM)
        server_process.wait(timeout=5) # Wait for server to terminate
        print("C server stopped.")
    except Exception as e:
        print(f"Error stopping server: {e}. Attempting to kill.")
        server_process.kill()
        server_process.wait(timeout=1)
        print("C server forcefully killed.")

@pytest.fixture(scope="function")
def redis_client(c_server):
    """
    Fixture to provide a Redis client for each test function.
    """
    r = redis.Redis(host=HOST, port=PORT, decode_responses=True)
    yield r
    r.connection_pool.disconnect() # Ensure client connection is closed

def test_ping(redis_client):
    """
    Test basic PING command.
    """
    response_ping = redis_client.ping()
    assert response_ping is True

def test_set_get(redis_client):
    """
    Test SET and GET commands.
    """
    key = "mykey"
    value = "myvalue"
    response_set = redis_client.set(key, value)
    assert response_set is True

    response_get = redis_client.get(key)
    assert response_get == value

def test_get_non_existent_key(redis_client):
    """
    Test GET command for a non-existent key.
    """
    non_existent_key = "nonexistentkey"
    response_get_non_existent = redis_client.get(non_existent_key)
    assert response_get_non_existent is None # Redis returns nil (None in Python) for non-existent keys

def test_pipeline_simple(redis_client):
    """
    Test simple command pipelining.
    """
    pipe = redis_client.pipeline()
    pipe.set("key1", "value1")
    pipe.get("key1")
    pipe.ping()
    responses_pipeline = pipe.execute()
    assert responses_pipeline == [True, "value1", True]

def test_hset_hget(redis_client):
    """
    Test HSET and HGET commands.
    """
    hash_key = "myhash"
    field1 = "field1"
    value1 = "value1"
    field2 = "field2"
    value2 = "value2"

    response_hset1 = redis_client.hset(hash_key, field1, value1)
    assert response_hset1 == 1 # New field added

    response_hset2 = redis_client.hset(hash_key, field2, value2)
    assert response_hset2 == 1 # New field added

    response_hget1 = redis_client.hget(hash_key, field1)
    assert response_hget1 == value1

    response_hget2 = redis_client.hget(hash_key, field2)
    assert response_hget2 == value2

def test_hget_non_existent_field(redis_client):
    """
    Test HGET command for a non-existent field in an existing hash key.
    """
    hash_key = "anotherhash"
    field = "existingfield"
    value = "somevalue"
    redis_client.hset(hash_key, field, value)

    non_existent_field = "nonexistentfield"
    response_hget = redis_client.hget(hash_key, non_existent_field)
    assert response_hget is None

def test_hget_non_existent_key(redis_client):
    """
    Test HGET command for a non-existent hash key.
    """
    non_existent_key = "nonexistenthash"
    field = "anyfield"
    response_hget = redis_client.hget(non_existent_key, field)
    assert response_hget is None

def test_hlen(redis_client):
    """
    Test HLEN command.
    """
    hash_key = "lenhash"
    redis_client.hset(hash_key, "f1", "v1")
    redis_client.hset(hash_key, "f2", "v2")
    redis_client.hset(hash_key, "f3", "v3")

    response_hlen = redis_client.hlen(hash_key)
    assert response_hlen == 3

def test_hlen_non_existent_key(redis_client):
    """
    Test HLEN command for a non-existent hash key.
    """
    response_hlen = redis_client.hlen("nonexistentlenhash")
    assert response_hlen == 0 # Redis returns 0 for non-existent hash keys

def test_hset_on_string_key(redis_client):
    """
    Test HSET command on a key that already holds a string.
    """
    key = "stringkey"
    value = "stringvalue"
    redis_client.set(key, value)

    # Attempt to HSET on a key that is already a string
    with pytest.raises(redis.exceptions.ResponseError): # Our current C implementation returns 0, which redis-py converts to ResponseError for commands that expect a specific return type.
        redis_client.hset(key, "field", "newvalue")

def test_get_on_hash_key(redis_client):
    """
    Test GET command on a key that holds a hash.
    """
    hash_key = "hashforget"
    field = "field1"
    value = "value1"
    redis_client.hset(hash_key, field, value)

    response_get = redis_client.get(hash_key)
    assert response_get is None # GET on a hash key should return None (nil)

# You can add more tests here for other commands, MULTI/EXEC etc.
