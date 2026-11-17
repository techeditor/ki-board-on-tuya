/* Standalone preview entry for the redesign shell. Lets `vite build` compile
 * the new UI and lets a reviewer open it in a browser without the Tauri runtime
 * (set_keymap logs instead of invoking). The production Tauri entry stays
 * src/main.tsx until the redesign replaces it in a later batch. */
import { createRoot } from "react-dom/client";

import { RedesignApp } from "./app/RedesignApp";

createRoot(document.getElementById("root")!).render(<RedesignApp />);
