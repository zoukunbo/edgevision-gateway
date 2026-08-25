# D33 Measurement V1 数据契约

## 数据流

模拟源/Modbus适配器 → measurement_t → JSON → MQTT

TCP只传输字节；frame通过LEN恢复完整payload。当前约定一帧承载一条完整Measurement JSON，不实现跨帧JSON分片。

## 字段规则

- schema_version：number，必须等于1
- device_id：string，非空，最多63字符
- sequence：number，每台设备独立递增，范围1..UINT32_MAX
- timestamp_ms：number，UTC Unix采样时间，单位毫秒，必须大于0
- metric：string，非空，最多31字符
- value：number，必须是有限数字
- unit：string，非空，最多15字符
- quality：string，仅允许good、uncertain、bad

读取超时且没有数据时不生成Measurement。

## JSON样例

```json
{"schema_version":1,"device_id":"sim-temperature-01","sequence":32,"timestamp_ms":1787623200123,"metric":"temperature","value":25.375,"unit":"celsius","quality":"good"}
```