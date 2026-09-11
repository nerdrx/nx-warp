# Queue priority with the retained large centre

Four 90-second full-field animation trials, priority on/off/off/on. All completed. Same 2688²-per-eye output and 1024px native centre as the [15-minute soak](../large-centre-soak/README.md). No code changes.

| Trial | Fresh selections/s | Decode GPU ms | Source offset proxy ms |
|---|---:|---:|---:|
| large-priority-on-a-client | 52.97 | 12.945 | 77.15 |
| large-priority-off-a-client | 52.73 | 13.130 | 77.32 |
| large-priority-off-b-client | 53.27 | 13.027 | 78.02 |
| large-priority-on-b-client | 53.21 | 13.093 | 77.64 |

Priority on averages 53.09 fresh selections/s versus 53.00 off; source-offset proxy is 77.40 versus 77.67 ms. These small differences do not establish a useful improvement. Priority **1 remains selected**. Earlier smaller-profile priority results do not establish the same benefit here.

Means use two-second summary windows after the initial 10 seconds. No physical motion-to-photon latency or per-frame percentiles are measured. Raw logs, status and scripts are in [raw](raw/).
