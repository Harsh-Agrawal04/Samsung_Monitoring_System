import sqlite3

DB_NAME = "sensor_data.db"


def get_connection():
    conn = sqlite3.connect(DB_NAME)
    conn.row_factory = sqlite3.Row
    return conn


def get_all_readings():
    """Return all rows as dictionaries, ready to parse in Python."""
    with get_connection() as conn:
        rows = conn.execute("""
            SELECT *
            FROM sensor_readings
            ORDER BY timestamp, id
        """).fetchall()
        return [dict(row) for row in rows]


def get_numeric_sensor_data():
    """Return sensor values suitable for math, graphing, or analysis."""
    with get_connection() as conn:
        rows = conn.execute("""
            SELECT timestamp, ip1, ip2, ip3,
                   sc_vac_read, lc_vac_read, lc_vac_status
            FROM sensor_readings
            ORDER BY timestamp, id
        """).fetchall()
        return [dict(row) for row in rows]


def get_valve_events(valve_name=None):
    """Return parsed valve events such as LV1 OPEN B."""
    with get_connection() as conn:
        if valve_name:
            rows = conn.execute("""
                SELECT timestamp, valve_name, valve_action,
                       valve_channel, valve_code
                FROM sensor_readings
                WHERE valve_name = ?
                  AND valve_action IS NOT NULL
                ORDER BY timestamp, id
            """, (valve_name.upper(),)).fetchall()
        else:
            rows = conn.execute("""
                SELECT timestamp, valve_name, valve_action,
                       valve_channel, valve_code
                FROM sensor_readings
                WHERE valve_name IS NOT NULL
                  AND valve_action IS NOT NULL
                ORDER BY timestamp, id
            """).fetchall()

        return [dict(row) for row in rows]


def get_readings_between(start_time, end_time):
    """Query a specific timestamp range."""
    with get_connection() as conn:
        rows = conn.execute("""
            SELECT *
            FROM sensor_readings
            WHERE timestamp BETWEEN ? AND ?
            ORDER BY timestamp, id
        """, (start_time, end_time)).fetchall()
        return [dict(row) for row in rows]


if __name__ == "__main__":
    readings = get_all_readings()

    print(f"Loaded {len(readings)} rows")

    print("\nFIRST 5 SENSOR ROWS")
    for row in readings[:5]:
        print(row)

    print("\nPARSED VALVE EVENTS")
    for event in get_valve_events()[:10]:
        print(event)

    print("\nTIME RANGE EXAMPLE")
    for row in get_readings_between(
        "2024-05-14 20:01:00",
        "2024-05-14 20:01:10"
    ):
        print(row)
