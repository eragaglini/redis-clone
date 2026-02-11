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

# You can add more tests here for other commands, MULTI/EXEC etc.
