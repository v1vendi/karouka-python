// loaded by external_module.html as <script type="module" src>.
import { answer } from "./external_module_dep.js";

const again = await import("./external_module_dep.js");
const ok = answer === 42 && again.answer === 42 && import.meta.url.startsWith("file:");
document.title = ok ? "external module ok" : "external module bad";
document.getElementById("status").textContent = document.title;
