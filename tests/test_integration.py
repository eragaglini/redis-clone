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
    Ensures a clean database state before each test.
    """
    r = redis.Redis(host=HOST, port=PORT, decode_responses=True)
    r.flushdb() # Clear the database before each test
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

    response_get = redis_client.get(hash_key)
    assert response_get is None # GET on a hash key should return None (nil)

def test_hdel_field(redis_client):
    """
    Test HDEL command for an existing field.
    """
    hash_key = "hdelhash"
    field1 = "f1"
    field2 = "f2"
    redis_client.hset(hash_key, field1, "v1")
    redis_client.hset(hash_key, field2, "v2")
    assert redis_client.hlen(hash_key) == 2

    response_hdel = redis_client.hdel(hash_key, field1)
    assert response_hdel == 1 # 1 field deleted
    assert redis_client.hlen(hash_key) == 1
    assert redis_client.hget(hash_key, field1) is None

    response_hdel_again = redis_client.hdel(hash_key, field1)
    assert response_hdel_again == 0 # Field already deleted

def test_hdel_non_existent_field(redis_client):
    """
    Test HDEL command for a non-existent field in an existing hash.
    """
    hash_key = "hdelnonexistent"
    redis_client.hset(hash_key, "f1", "v1")
    assert redis_client.hlen(hash_key) == 1

    response_hdel = redis_client.hdel(hash_key, "nonexistentfield")
    assert response_hdel == 0 # 0 fields deleted
    assert redis_client.hlen(hash_key) == 1 # Hash size should not change

def test_hdel_non_existent_key(redis_client):
    """
    Test HDEL command for a non-existent hash key.
    """
    response_hdel = redis_client.hdel("nonexistentkey", "anyfield")
    assert response_hdel == 0 # 0 fields deleted

def test_hdel_on_string_key(redis_client):
    """
    Test HDEL command on a key that holds a string.
    """
    key = "stringkeyhdel"
    redis_client.set(key, "somevalue")

    with pytest.raises(redis.exceptions.ResponseError):
        redis_client.hdel(key, "field")

def test_hgetall_basic(redis_client):
    """
    Test HGETALL command on a hash with multiple fields.
    """
    hash_key = "hgetallhash"
    redis_client.hset(hash_key, "f1", "v1")
    redis_client.hset(hash_key, "f2", "v2")
    redis_client.hset(hash_key, "f3", "v3")

    response_hgetall = redis_client.hgetall(hash_key)
    expected = {"f1": "v1", "f2": "v2", "f3": "v3"}
    assert response_hgetall == expected

def test_hgetall_empty_hash(redis_client):
    """
    Test HGETALL command on an empty hash.
    """
    hash_key = "emptyhash"
    # Create an empty hash by HSETting and then HDELing all fields
    redis_client.hset(hash_key, "temp_field", "temp_value")
    redis_client.hdel(hash_key, "temp_field")

    response_hgetall = redis_client.hgetall(hash_key)
    assert response_hgetall == {}

def test_hgetall_non_existent_key(redis_client):
    """
    Test HGETALL command on a non-existent key.
    """
    response_hgetall = redis_client.hgetall("nonexistenthgetallkey")
    assert response_hgetall == {}

def test_hgetall_on_string_key(redis_client):
    """
    Test HGETALL command on a key that holds a string.
    """
    key = "stringkeyhgetall"
    redis_client.set(key, "somevalue")

    with pytest.raises(redis.exceptions.ResponseError):
        redis_client.hgetall(key)

    with pytest.raises(redis.exceptions.ResponseError):
        redis_client.hgetall(key)

def test_hset_multiple_fields(redis_client):
    """
    Test HSET command with multiple field-value pairs.
    """
    hash_key = "multihash"
    # Set multiple fields at once using mapping
    response_hset = redis_client.hset(hash_key, mapping={"f1": "v1", "f2": "v2", "f3": "v3"})
    assert response_hset == 3 # 3 new fields added

    # Verify individual fields
    assert redis_client.hget(hash_key, "f1") == "v1"
    assert redis_client.hget(hash_key, "f2") == "v2"
    assert redis_client.hget(hash_key, "f3") == "v3"

    # Verify HLEN
    assert redis_client.hlen(hash_key) == 3

    # Update some fields and add a new one
    response_hset_update = redis_client.hset(hash_key, mapping={"f2": "new_v2", "f4": "v4"})
    assert response_hset_update == 1 # 1 new field added (f4)

    # Verify updated and new fields
    assert redis_client.hget(hash_key, "f2") == "new_v2"
    assert redis_client.hget(hash_key, "f4") == "v4"
    assert redis_client.hlen(hash_key) == 4

def test_hdel_multiple_fields(redis_client):
    """
    Test HDEL command with multiple field arguments.
    """
    hash_key = "multihdeldelhash"
    redis_client.hset(hash_key, mapping={"mf1": "mv1", "mf2": "mv2", "mf3": "mv3"})
    assert redis_client.hlen(hash_key) == 3

    # Delete multiple existing fields
    response_hdel = redis_client.hdel(hash_key, "mf1", "mf3")
    assert response_hdel == 2 # 2 fields deleted
    assert redis_client.hlen(hash_key) == 1
    assert redis_client.hget(hash_key, "mf1") is None
    assert redis_client.hget(hash_key, "mf2") == "mv2"
    assert redis_client.hget(hash_key, "mf3") is None

    # Delete a mix of existing and non-existing fields
    response_hdel_mix = redis_client.hdel(hash_key, "mf2", "mf4", "mf5")
    assert response_hdel_mix == 1 # Only mf2 was deleted
    assert redis_client.hlen(hash_key) == 0

    # Delete from a non-existent hash
    response_hdel_non_existent_hash = redis_client.hdel("nonexistenthash", "f1", "f2")
    assert response_hdel_non_existent_hash == 0

    # HDEL on a string key with multiple fields
    key = "stringkeyhdelmulti"
    redis_client.set(key, "somevalue")
    with pytest.raises(redis.exceptions.ResponseError):
        redis_client.hdel(key, "field1", "field2")

def test_del_single_key(redis_client):
    """
    Test DEL command for a single existing key.
    """
    key = "keytodel"
    redis_client.set(key, "value")
    assert redis_client.exists(key) == 1

    deleted_count = redis_client.delete(key)
    assert deleted_count == 1
    assert redis_client.exists(key) == 0

def test_del_multiple_keys(redis_client):
    """
    Test DEL command for multiple keys (string and hash).
    """
    key1 = "key1todel"
    key2 = "key2todel"
    hash_key = "hashkeytodel"

    redis_client.set(key1, "value1")
    redis_client.hset(hash_key, "f1", "v1")
    redis_client.set(key2, "value2")

    assert redis_client.exists(key1) == 1
    assert redis_client.exists(hash_key) == 1
    assert redis_client.exists(key2) == 1

    deleted_count = redis_client.delete(key1, hash_key, key2)
    assert deleted_count == 3
    assert redis_client.exists(key1) == 0
    assert redis_client.exists(hash_key) == 0
    assert redis_client.exists(key2) == 0

def test_del_non_existent_key(redis_client):
    """
    Test DEL command for a non-existent key.
    """
    deleted_count = redis_client.delete("nonexistentkeydel")
    assert deleted_count == 0

def test_del_mix_existent_non_existent(redis_client):
    """
    Test DEL command for a mix of existent and non-existent keys.
    """
    key1 = "mixkey1"
    key2 = "mixkey2"
    redis_client.set(key1, "val1")

    deleted_count = redis_client.delete(key1, "nonexistentmixkey", key2)
    assert deleted_count == 1 # Only key1 should be deleted
    assert redis_client.exists(key1) == 0
    assert redis_client.exists(key2) == 0 # Was never set

def test_exists_single_key(redis_client):
    """
    Test EXISTS command for an existing and non-existent key.
    """
    key = "existkey"
    redis_client.set(key, "value")

    assert redis_client.exists(key) == 1
    assert redis_client.exists("nonexistentexistkey") == 0

def test_type_string(redis_client):
    """
    Test TYPE command for a string key.
    """
    key = "stringtypekey"
    redis_client.set(key, "value")
    assert redis_client.type(key) == "string"

def test_type_hash(redis_client):
    """
    Test TYPE command for a hash key.
    """
    key = "hashtypekey"
    redis_client.hset(key, "field", "value")
    assert redis_client.type(key) == "hash"

def test_type_non_existent(redis_client):
    """
    Test TYPE command for a non-existent key.
    """
    assert redis_client.type("nonexistenttypekey") == "none"

# You can add more tests here for other commands, MULTI/EXEC etc.
