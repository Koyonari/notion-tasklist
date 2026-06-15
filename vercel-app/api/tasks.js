import { Client } from "@notionhq/client";

const notion = new Client({ auth: process.env.NOTION_TOKEN });

function dateInTz(isoString, tz) {
  return new Intl.DateTimeFormat("en-CA", {
    timeZone: tz,
    year: "numeric",
    month: "2-digit",
    day: "2-digit",
  }).format(new Date(isoString));
}

export default async function handler(req, res) {
  if (req.method !== "GET") return res.status(405).end();

  const tz = process.env.TIMEZONE || "UTC";
  const today = dateInTz(new Date().toISOString(), tz);

  try {
    const response = await notion.databases.query({
      database_id: process.env.NOTION_DATABASE_ID,
    });

    const tasks = response.results
      .map((page) => ({
        title: page.properties.Task?.title?.[0]?.plain_text ?? "Untitled",
        done: page.properties.Done?.checkbox ?? false,
        deadline: page.properties.Deadline?.date?.start ?? null,
        editedDate: dateInTz(page.last_edited_time, tz),
      }))
      // Hide completed tasks once the day they were completed has passed
      .filter((t) => !t.done || t.editedDate === today)
      .map(({ title, done, deadline }) => ({ title, done, deadline }))
      .sort((a, b) => {
        if (a.done !== b.done) return a.done ? 1 : -1;
        if (!a.deadline && !b.deadline) return 0;
        if (!a.deadline) return 1;
        if (!b.deadline) return -1;
        return a.deadline.localeCompare(b.deadline);
      });

    res.setHeader("Cache-Control", "no-store");
    res.status(200).json(tasks);
  } catch (err) {
    console.error(err);
    res.status(500).json({ error: "Failed to fetch tasks" });
  }
}
