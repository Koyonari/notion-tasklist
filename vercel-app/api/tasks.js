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

// status: 0 = todo, 1 = doing, 2 = done
function statusCode(name) {
  if (name === "Done") return 2;
  if (name === "Doing") return 1;
  return 0;
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
        id: page.id,
        title: page.properties.Task?.title?.[0]?.plain_text ?? "Untitled",
        status: statusCode(page.properties.Status?.status?.name ?? "To Do"),
        deadline: page.properties.Deadline?.date?.start ?? null,
        editedDate: dateInTz(page.last_edited_time, tz),
      }))
      // Hide Done tasks once the day they were completed has passed
      .filter((t) => t.status < 2 || t.editedDate === today)
      .map(({ id, title, status, deadline }) => ({ id, title, status, deadline }))
      .sort((a, b) => {
        if (a.status !== b.status) return a.status - b.status;
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
