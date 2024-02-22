SELECT reports.reported_at_ms, reports.dc_balance, COUNT(hotspot_connections.id), reports.fcnt, device_names.name, reports.battery_voltage, measurements.temperature, measurements.pressure, measurements.humidity
FROM ((((((reports
INNER JOIN app_eui ON app_eui.id = reports.app_eui_id)
INNER JOIN dev_eui ON dev_eui.id = reports.dev_eui_id)
INNER JOIN dev_addr ON dev_addr.id = reports.dev_addr_id)
INNER JOIN device_names ON device_names.id = reports.name_id)
INNER JOIN measurements ON measurements.report_id = reports.id)
INNER JOIN hotspot_connections ON hotspot_connections.report_id = reports.id)
GROUP BY reports.id;
