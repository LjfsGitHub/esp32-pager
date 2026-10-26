param(
  [Parameter(Mandatory = $true)][string]$Title,
  [string]$Body = "",
  [ValidateSet("info", "ok", "warn", "err")][string]$Level = "info",
  [int]$TtlMs = 6000
)

$payload = @{
  title  = $Title
  body   = $Body
  level  = $Level
  ttlMs  = $TtlMs
  speak  = $false
} | ConvertTo-Json -Compress

Invoke-RestMethod -Method Post `
  -Uri "http://127.0.0.1:8765/api/notify" `
  -ContentType "application/json; charset=utf-8" `
  -Body $payload
